// services/dual-writer/src/cql_server.cpp
//
// CQL Protocol v4 Proxy — transparent TCP proxy with write interception.
// Forwards raw CQL frames for reads; intercepts INSERT/UPDATE/DELETE for dual-writing.
//
// C++20 port of services/dual-writer/src/cql_server.rs
//
// Protocol reference: CQL binary protocol v4 (Apache Cassandra spec)
//   - Frame header: 9 bytes (version, flags, stream_id, opcode, body_length)
//   - Opcodes: 0x07=QUERY, 0x09=PREPARE, 0x0A=EXECUTE, 0x0D=BATCH
//   - Response version: 0x84 (0x80 | 0x04)
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_writer/writer.hpp"
#include "svckit/errors.hpp"

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dual_writer {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

// =============================================================================
// CQL Protocol Constants
// =============================================================================

static constexpr uint8_t CQL_VERSION_RESPONSE = 0x84;  // 0x80 | 0x04

// Opcodes
static constexpr uint8_t OP_ERROR         = 0x00;
static constexpr uint8_t OP_STARTUP       = 0x01;
static constexpr uint8_t OP_QUERY         = 0x07;
static constexpr uint8_t OP_RESULT        = 0x08;
static constexpr uint8_t OP_PREPARE       = 0x09;
static constexpr uint8_t OP_EXECUTE       = 0x0A;
static constexpr uint8_t OP_BATCH         = 0x0D;

// Result kinds
static constexpr int32_t RESULT_VOID      = 0x0001;
static constexpr int32_t RESULT_PREPARED  = 0x0004;

// =============================================================================
// Frame header (9 bytes)
// =============================================================================

struct FrameHeader {
    uint8_t  version{0};
    uint8_t  flags{0};
    int16_t  stream_id{0};
    uint8_t  opcode{0};
    int32_t  body_length{0};

    [[nodiscard]] std::array<uint8_t, 9> to_bytes() const noexcept {
        std::array<uint8_t, 9> buf{};
        buf[0] = version;
        buf[1] = flags;
        buf[2] = static_cast<uint8_t>((stream_id >> 8) & 0xFF);
        buf[3] = static_cast<uint8_t>(stream_id & 0xFF);
        buf[4] = opcode;
        buf[5] = static_cast<uint8_t>((body_length >> 24) & 0xFF);
        buf[6] = static_cast<uint8_t>((body_length >> 16) & 0xFF);
        buf[7] = static_cast<uint8_t>((body_length >> 8) & 0xFF);
        buf[8] = static_cast<uint8_t>(body_length & 0xFF);
        return buf;
    }

    static FrameHeader response(uint8_t opcode, int16_t stream_id, int32_t body_len) {
        return {CQL_VERSION_RESPONSE, 0x00, stream_id, opcode, body_len};
    }
};

// =============================================================================
// Helpers — big-endian parsing
// =============================================================================

static int32_t read_i32_be(const uint8_t* p) {
    return (static_cast<int32_t>(p[0]) << 24) |
           (static_cast<int32_t>(p[1]) << 16) |
           (static_cast<int32_t>(p[2]) << 8)  |
           static_cast<int32_t>(p[3]);
}

static int16_t read_i16_be(const uint8_t* p) {
    return static_cast<int16_t>((static_cast<int16_t>(p[0]) << 8) |
                                 static_cast<int16_t>(p[1]));
}

static FrameHeader parse_header(const std::array<uint8_t, 9>& buf) {
    FrameHeader h;
    h.version     = buf[0];
    h.flags       = buf[1];
    h.stream_id   = read_i16_be(&buf[2]);
    h.opcode      = buf[4];
    h.body_length = read_i32_be(&buf[5]);
    return h;
}

// =============================================================================
// CQL long string parser (4-byte length prefix)
// =============================================================================

static std::string parse_long_string(const std::vector<uint8_t>& body, size_t offset) {
    if (body.size() < offset + 4) return {};
    const auto len = static_cast<size_t>(read_i32_be(&body[offset]));
    if (body.size() < offset + 4 + len) return {};
    return {reinterpret_cast<const char*>(&body[offset + 4]), len};
}

// =============================================================================
// Query classification — is this a write?
// =============================================================================

static bool is_write_query(std::string_view query) {
    // Skip leading whitespace
    auto it = std::find_if(query.begin(), query.end(),
                           [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
    const auto trimmed = query.substr(static_cast<size_t>(it - query.begin()));

    auto starts_with_ci = [](std::string_view sv, std::string_view prefix) {
        if (sv.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(sv[i])) !=
                std::toupper(static_cast<unsigned char>(prefix[i])))
                return false;
        }
        return true;
    };

    return starts_with_ci(trimmed, "INSERT") ||
           starts_with_ci(trimmed, "UPDATE") ||
           starts_with_ci(trimmed, "DELETE");
}

// =============================================================================
// Extract keyspace.table from query
// =============================================================================

static std::string extract_table_target(std::string_view query) {
    // Find "INTO", "UPDATE", or "FROM" keyword, then take the next token
    std::string upper;
    upper.reserve(query.size());
    for (char c : query) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    size_t pos = std::string::npos;
    if (auto idx = upper.find("INTO");   idx != std::string::npos) pos = idx + 4;
    else if (auto idx2 = upper.find("UPDATE"); idx2 != std::string::npos) pos = idx2 + 6;
    else if (auto idx3 = upper.find("FROM");   idx3 != std::string::npos) pos = idx3 + 4;

    if (pos == std::string::npos) return "unknown";

    // Skip whitespace after keyword
    while (pos < query.size() && std::isspace(static_cast<unsigned char>(query[pos]))) ++pos;

    // Extract token until whitespace or (
    size_t end = pos;
    while (end < query.size() &&
           !std::isspace(static_cast<unsigned char>(query[end])) &&
           query[end] != '(' && query[end] != ';') {
        ++end;
    }

    return std::string{query.substr(pos, end - pos)};
}

// =============================================================================
// Build VOID result frame
// =============================================================================

static std::vector<uint8_t> build_void_result(int16_t stream_id) {
    // Body: result_kind = VOID (4 bytes)
    std::array<uint8_t, 4> body{};
    body[0] = 0x00; body[1] = 0x00; body[2] = 0x00; body[3] = 0x01;

    auto header = FrameHeader::response(OP_RESULT, stream_id, 4);
    auto hdr_bytes = header.to_bytes();

    std::vector<uint8_t> frame;
    frame.reserve(9 + 4);
    frame.insert(frame.end(), hdr_bytes.begin(), hdr_bytes.end());
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

// =============================================================================
// Read a complete CQL frame from a socket
// =============================================================================

static asio::awaitable<std::vector<uint8_t>>
read_full_frame(tcp::socket& sock) {
    std::array<uint8_t, 9> hdr_buf{};
    co_await asio::async_read(sock, asio::buffer(hdr_buf), asio::use_awaitable);

    const auto body_len = static_cast<size_t>(read_i32_be(&hdr_buf[5]));

    std::vector<uint8_t> frame;
    frame.reserve(9 + body_len);
    frame.insert(frame.end(), hdr_buf.begin(), hdr_buf.end());

    if (body_len > 0) {
        std::vector<uint8_t> body(body_len);
        co_await asio::async_read(sock, asio::buffer(body), asio::use_awaitable);
        frame.insert(frame.end(), body.begin(), body.end());
    }

    co_return frame;
}

// =============================================================================
// Forward frame to source and return raw response
// =============================================================================

static asio::awaitable<std::vector<uint8_t>>
forward_and_proxy(const FrameHeader& header,
                  const std::vector<uint8_t>& body,
                  tcp::socket& source_sock) {
    // Send original frame
    auto hdr_bytes = header.to_bytes();
    co_await asio::async_write(source_sock, asio::buffer(hdr_bytes), asio::use_awaitable);
    if (!body.empty()) {
        co_await asio::async_write(source_sock, asio::buffer(body), asio::use_awaitable);
    }

    // Read response
    co_return co_await read_full_frame(source_sock);
}

// =============================================================================
// Prepared statement tracking
// =============================================================================

struct PreparedStmt {
    std::string query;
    bool        is_write{false};
};

// =============================================================================
// Handle a single client connection — main proxy loop
// =============================================================================

static asio::awaitable<void>
handle_connection(tcp::socket client_sock,
                  std::shared_ptr<DualWriter> writer,
                  tcp::endpoint source_endpoint) {
    const auto client_addr = client_sock.remote_endpoint();
    spdlog::debug("Handling CQL connection from {}", client_addr.address().to_string());

    // Connect to source Cassandra
    auto executor = co_await asio::this_coro::executor;
    tcp::socket source_sock{executor};
    co_await source_sock.async_connect(source_endpoint, asio::use_awaitable);
    spdlog::debug("Connected to source at {}", source_endpoint.address().to_string());

    // Prepared statement tracker (per-connection)
    std::unordered_map<std::string, PreparedStmt> prepared_stmts;

    for (;;) {
        // Read frame header from client
        std::array<uint8_t, 9> hdr_buf{};
        try {
            co_await asio::async_read(client_sock, asio::buffer(hdr_buf), asio::use_awaitable);
        } catch (...) {
            spdlog::debug("Connection closed by client {}", client_addr.address().to_string());
            co_return;
        }

        const auto header = parse_header(hdr_buf);

        // Read body
        std::vector<uint8_t> body(static_cast<size_t>(header.body_length));
        if (header.body_length > 0) {
            co_await asio::async_read(client_sock, asio::buffer(body), asio::use_awaitable);
        }

        spdlog::debug("Frame: opcode=0x{:02x} stream={} body_len={}",
                       header.opcode, header.stream_id, header.body_length);

        std::vector<uint8_t> response;

        switch (header.opcode) {
            case OP_QUERY: {
                const auto query = parse_long_string(body, 0);
                spdlog::debug("QUERY: {}", query);

                if (is_write_query(query)) {
                    spdlog::debug("Write intercepted — dual-writing");
                    const auto table = extract_table_target(query);

                    auto write_result = co_await writer->write(query, table);

                    if (write_result.source_ok) {
                        response = build_void_result(header.stream_id);
                    } else {
                        spdlog::warn("Dual-write failed, falling back to source-only");
                        response = co_await forward_and_proxy(header, body, source_sock);
                    }
                } else {
                    response = co_await forward_and_proxy(header, body, source_sock);
                }
                break;
            }

            case OP_PREPARE: {
                const auto query = parse_long_string(body, 0);
                spdlog::debug("PREPARE: {}", query);

                // Forward to source to get prepared statement ID
                response = co_await forward_and_proxy(header, body, source_sock);

                // Track prepared statement for write detection on EXECUTE
                if (response.size() > 15 && response[4] == OP_RESULT) {
                    const auto result_kind = read_i32_be(&response[9]);
                    if (result_kind == RESULT_PREPARED) {
                        const auto id_len = static_cast<size_t>(read_i16_be(&response[13]));
                        if (response.size() >= 15 + id_len) {
                            std::string stmt_id(response.begin() + 15,
                                                response.begin() + 15 + static_cast<ptrdiff_t>(id_len));
                            prepared_stmts[stmt_id] = PreparedStmt{query, is_write_query(query)};
                            spdlog::debug("Tracked prepared stmt: is_write={}", is_write_query(query));
                        }
                    }
                }
                break;
            }

            case OP_EXECUTE: {
                // Extract prepared statement ID
                std::string stmt_id;
                if (body.size() >= 2) {
                    const auto id_len = static_cast<size_t>(read_i16_be(&body[0]));
                    if (body.size() >= 2 + id_len) {
                        stmt_id.assign(body.begin() + 2,
                                       body.begin() + 2 + static_cast<ptrdiff_t>(id_len));
                    }
                }

                auto it = prepared_stmts.find(stmt_id);
                if (it != prepared_stmts.end() && it->second.is_write) {
                    spdlog::debug("EXECUTE write stmt: {}", it->second.query);
                    const auto table = extract_table_target(it->second.query);

                    auto write_result = co_await writer->write(it->second.query, table);

                    if (write_result.source_ok) {
                        response = build_void_result(header.stream_id);
                    } else {
                        response = co_await forward_and_proxy(header, body, source_sock);
                    }
                } else {
                    response = co_await forward_and_proxy(header, body, source_sock);
                }
                break;
            }

            case OP_BATCH: {
                // Forward batches to source; full batch parsing for dual-write is complex
                spdlog::debug("BATCH — forwarding to source");
                response = co_await forward_and_proxy(header, body, source_sock);
                break;
            }

            default: {
                // All other opcodes: transparent proxy
                response = co_await forward_and_proxy(header, body, source_sock);
                break;
            }
        }

        // Send response to client
        co_await asio::async_write(client_sock, asio::buffer(response), asio::use_awaitable);
    }
}

// =============================================================================
// start_cql_server — accept loop (called from main.cpp)
// =============================================================================

asio::awaitable<void>
start_cql_server(asio::io_context& ioc,
                 std::shared_ptr<DualWriter> writer,
                 const std::string& bind_addr,
                 uint16_t bind_port,
                 const std::string& source_host,
                 uint16_t source_port) {
    auto executor = co_await asio::this_coro::executor;

    tcp::endpoint listen_ep{asio::ip::make_address(bind_addr), bind_port};
    tcp::acceptor acceptor{executor, listen_ep};
    acceptor.set_option(tcp::acceptor::reuse_address(true));

    tcp::endpoint source_ep{asio::ip::make_address(source_host), source_port};

    spdlog::info("CQL proxy listening on {}:{}", bind_addr, bind_port);
    spdlog::info("Proxying to source at {}:{}", source_host, source_port);

    for (;;) {
        tcp::socket client = co_await acceptor.async_accept(asio::use_awaitable);
        spdlog::info("New CQL connection from {}",
                     client.remote_endpoint().address().to_string());

        asio::co_spawn(executor,
                       handle_connection(std::move(client), writer, source_ep),
                       asio::detached);
    }
}

} // namespace dual_writer

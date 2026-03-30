// services/dual-writer/src/main.cpp
//
// Dual-Writer entry point — CQL proxy with dual-write interception.
//
// C++20 port of services/dual-writer/src/main.rs
//
// Architecture:
//   - Boost.Asio io_context drives the CQL proxy (async TCP accept loop)
//   - std::jthread for HTTP API server (cpp-httplib, blocking)
//   - std::jthread for filter config hot-reload (60s interval)
//   - co_spawn wires the CQL server coroutine into the Asio event loop
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_writer/config.hpp"
#include "dual_writer/filter.hpp"
#include "dual_writer/writer.hpp"
#include "svckit/database.hpp"
#include "svckit/metrics.hpp"

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace dual_writer {

// Forward declarations from cql_server.cpp and api.cpp
boost::asio::awaitable<void>
start_cql_server(boost::asio::io_context& ioc,
                 std::shared_ptr<DualWriter> writer,
                 const std::string& bind_addr,
                 uint16_t bind_port,
                 const std::string& source_host,
                 uint16_t source_port);

void start_api_server(uint16_t port,
                      std::shared_ptr<svckit::ScyllaConnection> source,
                      std::shared_ptr<svckit::ScyllaConnection> target,
                      std::shared_ptr<BlacklistFilterGovernor>  filter,
                      std::shared_ptr<svckit::MetricsRegistry>  metrics);

} // namespace dual_writer

// =============================================================================
// Argument parsing (minimal — mirrors Rust clap)
// =============================================================================

struct Args {
    std::string config_path{"config/dual-writer-scylla.yaml"};
    std::string filter_config_path{"config/filter-rules.yaml"};
    std::string bind_addr{"0.0.0.0"};
    uint16_t    bind_port{9042};
    uint16_t    metrics_port{9090};
};

static Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        if ((arg == "--config" || arg == "-c") && i + 1 < argc)
            args.config_path = argv[++i];
        else if (arg == "--filter-config" && i + 1 < argc)
            args.filter_config_path = argv[++i];
        else if (arg == "--bind-addr" && i + 1 < argc) {
            const std::string addr{argv[++i]};
            const auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                args.bind_addr = addr.substr(0, colon);
                args.bind_port = static_cast<uint16_t>(std::stoi(addr.substr(colon + 1)));
            } else {
                args.bind_addr = addr;
            }
        }
        else if (arg == "--metrics-port" && i + 1 < argc)
            args.metrics_port = static_cast<uint16_t>(std::stoi(argv[++i]));
    }
    return args;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    spdlog::info("Starting CQL Dual-Writer Proxy");

    const auto args = parse_args(argc, argv);

    try {
        // --- Load configuration ---
        auto config = dual_writer::DualWriterConfig::from_yaml(args.config_path);

        // --- Load filter rules ---
        auto filter_config = dual_writer::load_filter_config(args.filter_config_path);
        auto filter = std::make_shared<dual_writer::BlacklistFilterGovernor>(filter_config);

        // --- Connect to clusters ---
        auto source_conn = std::make_shared<svckit::ScyllaConnection>(config.source);
        auto target_conn = std::make_shared<svckit::ScyllaConnection>(config.target);

        // --- Metrics registry ---
        auto metrics = std::make_shared<svckit::MetricsRegistry>("dual_writer");

        // --- Build DualWriter ---
        auto writer = std::make_shared<dual_writer::DualWriter>(
            config, source_conn, target_conn, filter, metrics);

        spdlog::info("Dual-writer initialized");
        spdlog::info("  Source: {} hosts, keyspace={}",
                     config.source.hosts.size(), config.source.keyspace);
        spdlog::info("  Target: {} hosts, keyspace={}",
                     config.target.hosts.size(), config.target.keyspace);

        // --- Source address for CQL proxying ---
        const auto& source_host = config.source.hosts.empty()
                                      ? "127.0.0.1"
                                      : config.source.hosts.front();
        const auto source_port = config.source.port;

        // --- Start HTTP API in background thread ---
        std::jthread api_thread([&args, source_conn, target_conn, filter, metrics]() {
            dual_writer::start_api_server(
                args.metrics_port, source_conn, target_conn, filter, metrics);
        });

        // --- Start filter hot-reload in background thread ---
        std::jthread reload_thread([&args, filter](std::stop_token stoken) {
            while (!stoken.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
                if (stoken.stop_requested()) break;
                try {
                    filter->reload_config();
                    spdlog::debug("Filter config hot-reload complete");
                } catch (const std::exception& e) {
                    spdlog::warn("Filter hot-reload failed: {}", e.what());
                }
            }
        });

        // --- Boost.Asio event loop for CQL proxy ---
        boost::asio::io_context ioc{
            static_cast<int>(std::thread::hardware_concurrency())};

        // Signal handling
        boost::asio::signal_set signals{ioc, SIGINT, SIGTERM};
        signals.async_wait([&ioc](auto, auto) {
            spdlog::info("Shutdown signal received");
            ioc.stop();
        });

        // Spawn CQL server coroutine
        spdlog::info("Starting CQL proxy on {}:{}", args.bind_addr, args.bind_port);
        spdlog::info("  Proxying reads to source: {}:{}", source_host, source_port);
        spdlog::info("  Intercepting writes for dual-write");

        boost::asio::co_spawn(
            ioc,
            dual_writer::start_cql_server(
                ioc, writer,
                args.bind_addr, args.bind_port,
                source_host, source_port),
            boost::asio::detached);

        // Run event loop — blocks until ioc.stop() from signal handler
        ioc.run();

        spdlog::info("Dual-writer shutdown complete");
        return 0;

    } catch (const svckit::SyncError& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    } catch (const std::exception& e) {
        spdlog::critical("Unexpected error: {}", e.what());
        return 1;
    }
}

#pragma once
// services/dual-writer/include/dual_writer/writer.hpp
//
// DualWriter — fans writes to both source and target clusters simultaneously.
// Uses Boost.Asio coroutines (co_await) for async execution.
// C++20 port of services/dual-writer/src/writer.rs

#include "dual_writer/config.hpp"
#include "dual_writer/filter.hpp"
#include "svckit/database.hpp"
#include "svckit/errors.hpp"
#include "svckit/metrics.hpp"
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace dual_writer {

namespace asio = boost::asio;

// ---------------------------------------------------------------------------
// WriteResult — outcome of a dual write operation
// ---------------------------------------------------------------------------
struct WriteResult {
    bool        source_ok{false};
    bool        target_ok{false};
    std::string error_message;

    [[nodiscard]] bool fully_successful() const noexcept {
        return source_ok && target_ok;
    }
};

// ---------------------------------------------------------------------------
// DualWriter
//
// Rule of 5:
//   - Copy: DELETED — owns two live database connections
//   - Move: DELETED — connections hold internal thread state (Asio strands)
//   - Dtor: defined — graceful shutdown of in-flight writes
// ---------------------------------------------------------------------------
class DualWriter {
public:
    DualWriter(DualWriterConfig                       config,
               std::shared_ptr<svckit::ScyllaConnection> source,
               std::shared_ptr<svckit::ScyllaConnection> target,
               std::shared_ptr<BlacklistFilterGovernor>  filter,
               std::shared_ptr<svckit::MetricsRegistry>  metrics);

    DualWriter(const DualWriter&)            = delete;
    DualWriter& operator=(const DualWriter&) = delete;
    DualWriter(DualWriter&&)                 = delete;
    DualWriter& operator=(DualWriter&&)      = delete;
    ~DualWriter();

    // Execute a CQL write to both source and target concurrently.
    // Returns a Boost.Asio awaitable — call with co_await from a coroutine.
    [[nodiscard]] asio::awaitable<WriteResult>
    write(std::string_view cql, std::string_view keyspace) const;

    // Health check on both connections
    [[nodiscard]] asio::awaitable<bool> health_check() const;

private:
    DualWriterConfig                           config_;
    std::shared_ptr<svckit::ScyllaConnection>  source_;
    std::shared_ptr<svckit::ScyllaConnection>  target_;
    std::shared_ptr<BlacklistFilterGovernor>   filter_;
    std::shared_ptr<svckit::MetricsRegistry>   metrics_;
};

} // namespace dual_writer

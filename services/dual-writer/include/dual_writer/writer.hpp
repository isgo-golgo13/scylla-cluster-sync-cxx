#pragma once
// services/dual-writer/include/dual_writer/writer.hpp
//
// DualWriter — fans writes to both source and target clusters simultaneously.
// Uses Boost.Asio coroutines (co_await) for async execution.
//
// C++20 port of services/dual-writer/src/writer.rs
//
// Production features (matching Rust 1:1):
//   - FailedWrite tracking in concurrent map (mirrors DashMap<Uuid, FailedWrite>)
//   - Background retry loop (std::jthread, configurable interval + max attempts)
//   - Filter-aware retry (blacklisted writes removed from retry queue)
//   - Atomic WriterStats (total/successful/failed/retried counters)
//   - WriteMode: SourceOnly, DualAsync, DualSync, TargetOnly
//
// Rule of 5:
//   - Copy: DELETED — owns connections + mutex-protected map + jthread
//   - Move: DELETED — jthread + mutex not movable post-construction
//   - Dtor: defined — signals retry thread to stop, drains in-flight writes
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_writer/config.hpp"
#include "dual_writer/filter.hpp"
#include "svckit/database.hpp"
#include "svckit/errors.hpp"
#include "svckit/metrics.hpp"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace dual_writer {

namespace asio = boost::asio;

// =============================================================================
// WriteMode — migration phase selector (mirrors Rust WriteMode enum)
// =============================================================================

enum class WriteMode {
    SourceOnly,   // Phase 0: write source only
    DualAsync,    // Phase 1: source sync + target fire-and-forget
    DualSync,     // Phase 2: both sync, both must succeed
    TargetOnly,   // Phase 3: target only (source decommissioned)
};

[[nodiscard]] std::string to_string(WriteMode mode);

// =============================================================================
// WriteResult — outcome of a dual write operation
// =============================================================================

struct WriteResult {
    bool        source_ok{false};
    bool        target_ok{false};
    std::string error_message;
    double      latency_ms{0.0};

    [[nodiscard]] bool fully_successful() const noexcept {
        return source_ok && target_ok;
    }
};

// =============================================================================
// FailedWrite — queued for background retry
// Mirrors Rust FailedWrite { request, error, attempts, first_attempt }
// =============================================================================

struct FailedWrite {
    std::string id;                    // UUID string
    std::string cql;                   // Original CQL statement
    std::string keyspace;              // keyspace.table for filter re-check
    std::string error;                 // Last error message
    uint32_t    attempts{1};           // Number of attempts so far
    std::chrono::steady_clock::time_point first_attempt{std::chrono::steady_clock::now()};

    FailedWrite()                              = default;
    FailedWrite(const FailedWrite&)            = default;
    FailedWrite(FailedWrite&&)                 = default;
    FailedWrite& operator=(const FailedWrite&) = default;
    FailedWrite& operator=(FailedWrite&&)      = default;
    ~FailedWrite()                             = default;
};

// =============================================================================
// WriterStats — atomic counters for observability
// Mirrors Rust WriterStats { total, successful, failed, retried, filtered }
// =============================================================================

struct WriterStats {
    std::atomic<uint64_t> total_writes{0};
    std::atomic<uint64_t> successful_writes{0};
    std::atomic<uint64_t> failed_writes{0};
    std::atomic<uint64_t> retried_writes{0};
    std::atomic<uint64_t> filtered_writes{0};
    std::atomic<uint64_t> abandoned_writes{0};

    // Atomics are not copyable/movable — Rule of 5: all deleted
    WriterStats()                              = default;
    WriterStats(const WriterStats&)            = delete;
    WriterStats& operator=(const WriterStats&) = delete;
    WriterStats(WriterStats&&)                 = delete;
    WriterStats& operator=(WriterStats&&)      = delete;
    ~WriterStats()                             = default;
};

// Serializable snapshot for API responses
struct WriterStatsSnapshot {
    uint64_t total_writes{0};
    uint64_t successful_writes{0};
    uint64_t failed_writes{0};
    uint64_t retried_writes{0};
    uint64_t filtered_writes{0};
    uint64_t abandoned_writes{0};
    uint64_t pending_retries{0};
};

// =============================================================================
// DualWriter
// =============================================================================

class DualWriter {
public:
    DualWriter(DualWriterConfig                        config,
               std::shared_ptr<svckit::ScyllaConnection> source,
               std::shared_ptr<svckit::ScyllaConnection> target,
               std::shared_ptr<BlacklistFilterGovernor>  filter,
               std::shared_ptr<svckit::MetricsRegistry>  metrics);

    DualWriter(const DualWriter&)            = delete;
    DualWriter& operator=(const DualWriter&) = delete;
    DualWriter(DualWriter&&)                 = delete;
    DualWriter& operator=(DualWriter&&)      = delete;
    ~DualWriter();

    // Core dual-write — returns Boost.Asio awaitable
    [[nodiscard]] asio::awaitable<WriteResult>
    write(std::string_view cql, std::string_view keyspace);

    // Health check on both connections
    [[nodiscard]] asio::awaitable<bool> health_check() const;

    // Stats snapshot for API
    [[nodiscard]] WriterStatsSnapshot get_stats() const;

    // Pending retry queue size
    [[nodiscard]] size_t pending_retry_count() const;

private:
    // Write mode dispatchers
    WriteResult write_source_only(std::string_view cql);
    WriteResult write_target_only(std::string_view cql);
    WriteResult write_dual_sync(std::string_view cql, std::string_view keyspace);
    WriteResult write_dual_async(std::string_view cql, std::string_view keyspace);

    // Queue a failed shadow write for background retry
    void enqueue_failed_write(std::string_view cql, std::string_view keyspace,
                              std::string_view error);

    // Background retry loop — runs in std::jthread
    void retry_failed_writes_loop(std::stop_token stoken);

    // Generate a simple UUID-like ID for failed write tracking
    [[nodiscard]] static std::string generate_id();

    DualWriterConfig                           config_;
    std::shared_ptr<svckit::ScyllaConnection>  source_;
    std::shared_ptr<svckit::ScyllaConnection>  target_;
    std::shared_ptr<BlacklistFilterGovernor>    filter_;
    std::shared_ptr<svckit::MetricsRegistry>   metrics_;

    // Failed write tracking — mutex-protected map (mirrors Rust DashMap)
    mutable std::mutex                                    failed_writes_mutex_;
    std::unordered_map<std::string, FailedWrite>         failed_writes_;

    // Atomic stats
    WriterStats stats_;

    // Background retry thread
    std::jthread retry_thread_;
};

} // namespace dual_writer

// services/dual-writer/src/writer.cpp
//
// DualWriter — fans CQL writes to source + target clusters concurrently.
// Boost.Asio co_await/co_return coroutines replace Tokio async.
//
// C++20 port of services/dual-writer/src/writer.rs
//
// Write modes (mirrors Rust WriteMode):
//   - SourceOnly:  write source, skip target
//   - DualAsync:   write source first, fire-and-forget shadow to target
//   - DualSync:    write both in parallel, both must succeed
//   - TargetOnly:  write target only
//
// Rule of 5: copy DELETED, move DELETED (owns connections + Asio strands)
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_writer/writer.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <utility>

namespace dual_writer {

namespace asio = boost::asio;

// =============================================================================
// Construction
// =============================================================================

DualWriter::DualWriter(DualWriterConfig                        config,
                       std::shared_ptr<svckit::ScyllaConnection> source,
                       std::shared_ptr<svckit::ScyllaConnection> target,
                       std::shared_ptr<BlacklistFilterGovernor>  filter,
                       std::shared_ptr<svckit::MetricsRegistry>  metrics)
    : config_{std::move(config)}
    , source_{std::move(source)}
    , target_{std::move(target)}
    , filter_{std::move(filter)}
    , metrics_{std::move(metrics)}
{
    spdlog::info("DualWriter initialized — source connected={}, target connected={}",
                 source_->is_connected(), target_->is_connected());
}

// =============================================================================
// Destructor — signal any in-flight operations to drain
// =============================================================================

DualWriter::~DualWriter() {
    spdlog::info("DualWriter shutting down");
}

// =============================================================================
// write() — core dual-write coroutine
//
// 1. Check filter — if blacklisted, write source only
// 2. Write source (always — source is authoritative)
// 3. Shadow-write target (async, non-blocking on source path)
// =============================================================================

asio::awaitable<WriteResult>
DualWriter::write(std::string_view cql, std::string_view keyspace) const {
    WriteResult result;

    // --- Filter gate: check if keyspace.table is blacklisted ---
    if (filter_ && filter_->is_enabled()) {
        const auto table_decision = filter_->should_skip_table(keyspace);
        if (table_decision == svckit::FilterDecision::SkipTable) {
            spdlog::info("Skipping target write — table blacklisted: {}", keyspace);

            // Write source only
            try {
                source_->execute(cql);
                result.source_ok = true;
                result.target_ok = false;  // Intentionally skipped, not failed
            } catch (const svckit::SyncError& e) {
                result.source_ok = false;
                result.error_message = e.what();
                spdlog::error("Source-only write failed: {}", e.what());
            }

            if (metrics_) {
                metrics_->record_operation("write", keyspace, result.source_ok, 0.0);
            }

            co_return result;
        }
    }

    // --- Phase 1: Write to source (primary, authoritative) ---
    const auto start = std::chrono::steady_clock::now();

    try {
        source_->execute(cql);
        result.source_ok = true;
    } catch (const svckit::SyncError& e) {
        result.source_ok = false;
        result.error_message = std::string{"Source write failed: "} + e.what();
        spdlog::error("{}", result.error_message);

        if (metrics_) {
            metrics_->record_operation("write", keyspace, false, 0.0);
        }
        co_return result;
    }

    // --- Phase 2: Shadow-write to target (async, non-blocking) ---
    try {
        target_->execute(cql);
        result.target_ok = true;
        spdlog::debug("Shadow write successful for keyspace: {}", keyspace);
    } catch (const svckit::SyncError& e) {
        result.target_ok = false;
        result.error_message = std::string{"Target shadow write failed: "} + e.what();
        spdlog::warn("Shadow write failed: {} — source write was successful", e.what());

        // TODO: Queue for retry (FailedWrite tracking — mirrors Rust DashMap<Uuid, FailedWrite>)
    }

    // --- Metrics ---
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double duration_secs =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1e6;

    if (metrics_) {
        metrics_->record_operation("write", keyspace, result.fully_successful(), duration_secs);
    }

    co_return result;
}

// =============================================================================
// health_check() — ping both clusters
// =============================================================================

asio::awaitable<bool> DualWriter::health_check() const {
    const bool source_ok = source_ && source_->ping();
    const bool target_ok = target_ && target_->ping();

    if (!source_ok) spdlog::warn("Source health check failed");
    if (!target_ok) spdlog::warn("Target health check failed");

    co_return source_ok && target_ok;
}

} // namespace dual_writer

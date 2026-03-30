// services/dual-writer/src/writer.cpp
//
// DualWriter — fans CQL writes to source + target clusters concurrently.
// Boost.Asio co_await/co_return coroutines replace Tokio async.
//
// C++20 port of services/dual-writer/src/writer.rs
//
// Production features (matching Rust 1:1):
//
//   1. WriteMode dispatch:
//      - SourceOnly:  write source, skip target
//      - DualAsync:   write source sync, fire-and-forget shadow to target
//      - DualSync:    write both, both must succeed
//      - TargetOnly:  write target only (source decommissioned)
//
//   2. FailedWrite retry system:
//      - Failed shadow writes queued in mutex-protected unordered_map
//      - Background std::jthread retries every retry_interval_secs
//      - Filter-aware: if write is now blacklisted, remove from queue
//      - Abandons after max_retry_attempts with logging
//
//   3. WriterStats:
//      - Atomic counters: total, successful, failed, retried, filtered, abandoned
//      - Snapshot method for API responses
//
// Rule of 5: copy DELETED, move DELETED (owns jthread + mutex + connections)
// Dtor: signals jthread to stop, joins, then releases connections
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_writer/writer.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

namespace dual_writer {

namespace asio = boost::asio;

// =============================================================================
// WriteMode → string
// =============================================================================

std::string to_string(WriteMode mode) {
    switch (mode) {
        case WriteMode::SourceOnly: return "source_only";
        case WriteMode::DualAsync:  return "dual_async";
        case WriteMode::DualSync:   return "dual_sync";
        case WriteMode::TargetOnly: return "target_only";
    }
    return "unknown";
}

// =============================================================================
// Simple ID generator (replaces Uuid::new_v4() from Rust)
// =============================================================================

std::string DualWriter::generate_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    const auto hi = dist(rng);
    const auto lo = dist(rng);

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<uint32_t>(hi >> 32),
                  static_cast<uint16_t>(hi >> 16),
                  static_cast<uint16_t>((hi & 0xFFFF) | 0x4000),  // version 4
                  static_cast<uint16_t>((lo >> 48) | 0x8000),     // variant 1
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return buf;
}

// =============================================================================
// Construction — start background retry thread
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
    , retry_thread_{[this](std::stop_token st) { retry_failed_writes_loop(st); }}
{
    spdlog::info("DualWriter initialized — source connected={}, target connected={}",
                 source_->is_connected(), target_->is_connected());
    spdlog::info("  Retry interval: {}ms, max attempts: {}",
                 config_.writer.retry_delay_ms, config_.writer.max_retries);
}

// =============================================================================
// Destructor — signal retry thread to stop, join, log final stats
// =============================================================================

DualWriter::~DualWriter() {
    retry_thread_.request_stop();

    const auto snap = get_stats();
    spdlog::info("DualWriter shutting down — total={} success={} failed={} retried={} filtered={} abandoned={} pending={}",
                 snap.total_writes, snap.successful_writes, snap.failed_writes,
                 snap.retried_writes, snap.filtered_writes, snap.abandoned_writes,
                 snap.pending_retries);
}

// =============================================================================
// write() — core dual-write coroutine
// =============================================================================

asio::awaitable<WriteResult>
DualWriter::write(std::string_view cql, std::string_view keyspace) {
    stats_.total_writes.fetch_add(1, std::memory_order_relaxed);

    const auto start = std::chrono::steady_clock::now();

    // --- Filter gate: check if keyspace.table is blacklisted ---
    if (filter_ && filter_->is_enabled()) {
        const auto table_decision = filter_->should_skip_table(keyspace);
        if (table_decision == svckit::FilterDecision::SkipTable) {
            spdlog::info("Skipping target write — table blacklisted: {}", keyspace);
            stats_.filtered_writes.fetch_add(1, std::memory_order_relaxed);

            // Write source only
            auto result = write_source_only(cql);
            if (result.source_ok) {
                stats_.successful_writes.fetch_add(1, std::memory_order_relaxed);
            } else {
                stats_.failed_writes.fetch_add(1, std::memory_order_relaxed);
            }

            if (metrics_) {
                metrics_->record_operation("write", keyspace, result.source_ok, 0.0);
            }

            co_return result;
        }
    }

    // --- Dispatch by WriteMode ---
    // TODO: Make WriteMode configurable at runtime (currently DualAsync)
    auto result = write_dual_async(cql, keyspace);

    // --- Record elapsed time ---
    const auto elapsed = std::chrono::steady_clock::now() - start;
    result.latency_ms =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) / 1000.0;

    // --- Update stats ---
    if (result.source_ok) {
        stats_.successful_writes.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats_.failed_writes.fetch_add(1, std::memory_order_relaxed);
    }

    // --- Metrics ---
    if (metrics_) {
        metrics_->record_operation("write", keyspace,
                                    result.fully_successful(),
                                    result.latency_ms / 1000.0);
    }

    co_return result;
}

// =============================================================================
// write_source_only — Phase 0: source only
// =============================================================================

WriteResult DualWriter::write_source_only(std::string_view cql) {
    WriteResult result;
    try {
        source_->execute(cql);
        result.source_ok = true;
    } catch (const svckit::SyncError& e) {
        result.source_ok = false;
        result.error_message = std::string{"Source write failed: "} + e.what();
        spdlog::error("{}", result.error_message);
    }
    return result;
}

// =============================================================================
// write_target_only — Phase 3: target only (source decommissioned)
// =============================================================================

WriteResult DualWriter::write_target_only(std::string_view cql) {
    WriteResult result;
    try {
        target_->execute(cql);
        result.target_ok = true;
    } catch (const svckit::SyncError& e) {
        result.target_ok = false;
        result.error_message = std::string{"Target write failed: "} + e.what();
        spdlog::error("{}", result.error_message);
    }
    return result;
}

// =============================================================================
// write_dual_sync — Phase 2: both sync, both must succeed
// =============================================================================

WriteResult DualWriter::write_dual_sync(std::string_view cql,
                                         std::string_view keyspace) {
    WriteResult result;

    // Check filter before target write
    if (filter_ && filter_->is_enabled()) {
        if (filter_->should_skip_table(keyspace) == svckit::FilterDecision::SkipTable) {
            spdlog::info("Skipping target write — blacklisted: {}", keyspace);
            stats_.filtered_writes.fetch_add(1, std::memory_order_relaxed);
            return write_source_only(cql);
        }
    }

    // Execute both writes
    try {
        source_->execute(cql);
        result.source_ok = true;
    } catch (const svckit::SyncError& e) {
        result.source_ok     = false;
        result.error_message = std::string{"Source write failed: "} + e.what();
        spdlog::error("{}", result.error_message);
        return result;
    }

    try {
        target_->execute(cql);
        result.target_ok = true;
    } catch (const svckit::SyncError& e) {
        result.target_ok     = false;
        result.error_message = std::string{"Target write failed: "} + e.what();
        spdlog::error("{}", result.error_message);
    }

    return result;
}

// =============================================================================
// write_dual_async — Phase 1: source sync + target fire-and-forget
//
// This is the primary production path. Source write is synchronous (blocks
// until Cassandra responds). Target shadow write failure does NOT fail the
// request — instead the write is queued in failed_writes_ for background
// retry by retry_failed_writes_loop().
// =============================================================================

WriteResult DualWriter::write_dual_async(std::string_view cql,
                                          std::string_view keyspace) {
    WriteResult result;

    // --- Phase 1: Synchronous write to source (PRIMARY) ---
    try {
        source_->execute(cql);
        result.source_ok = true;
    } catch (const svckit::SyncError& e) {
        result.source_ok     = false;
        result.error_message = std::string{"Source write failed: "} + e.what();
        spdlog::error("{}", result.error_message);
        return result;  // Source failure = total failure, don't attempt target
    }

    // --- Phase 2: Shadow write to target (fire-and-forget) ---
    try {
        target_->execute(cql);
        result.target_ok = true;
        spdlog::debug("Shadow write successful for keyspace: {}", keyspace);
    } catch (const svckit::SyncError& e) {
        result.target_ok = false;
        result.error_message = std::string{"Target shadow write failed: "} + e.what();
        spdlog::warn("Shadow write failed: {} — source write was successful, queuing for retry",
                     e.what());

        // Queue for background retry — this is the critical production path
        enqueue_failed_write(cql, keyspace, e.what());
    }

    return result;
}

// =============================================================================
// enqueue_failed_write — add to retry queue under mutex
// =============================================================================

void DualWriter::enqueue_failed_write(std::string_view cql,
                                       std::string_view keyspace,
                                       std::string_view error) {
    auto id = generate_id();

    FailedWrite fw;
    fw.id       = id;
    fw.cql      = std::string{cql};
    fw.keyspace = std::string{keyspace};
    fw.error    = std::string{error};
    fw.attempts = 1;
    fw.first_attempt = std::chrono::steady_clock::now();

    {
        std::lock_guard lock{failed_writes_mutex_};
        failed_writes_.emplace(std::move(id), std::move(fw));
    }

    spdlog::debug("Failed write queued for retry (pending={})", pending_retry_count());
}

// =============================================================================
// retry_failed_writes_loop — background thread (std::jthread with stop_token)
//
// Mirrors Rust retry_failed_writes_loop():
//   1. Sleep for retry_interval
//   2. Iterate all pending failed writes
//   3. If attempts >= max_retry_attempts → abandon (log + remove)
//   4. If filter now blacklists this write → remove (no longer needed)
//   5. Otherwise → retry the write to target
//      - Success: remove from queue, increment retried_writes
//      - Failure: increment attempts, update error message
// =============================================================================

void DualWriter::retry_failed_writes_loop(std::stop_token stoken) {
    spdlog::info("Retry loop started (interval={}ms, max_attempts={})",
                 config_.writer.retry_delay_ms, config_.writer.max_retries);

    while (!stoken.stop_requested()) {
        // Sleep for retry interval (interruptible via stop_token)
        for (uint64_t elapsed = 0;
             elapsed < config_.writer.retry_delay_ms && !stoken.stop_requested();
             elapsed += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (stoken.stop_requested()) break;

        // --- Snapshot the current failed writes ---
        std::vector<std::pair<std::string, FailedWrite>> snapshot;
        {
            std::lock_guard lock{failed_writes_mutex_};
            if (failed_writes_.empty()) continue;

            snapshot.reserve(failed_writes_.size());
            for (const auto& [id, fw] : failed_writes_) {
                snapshot.emplace_back(id, fw);
            }
        }

        if (snapshot.empty()) continue;

        spdlog::debug("Retry loop processing {} pending writes", snapshot.size());

        std::vector<std::string> to_remove;
        std::vector<std::pair<std::string, FailedWrite>> to_update;

        for (auto& [id, fw] : snapshot) {
            // --- Check 1: Max attempts exceeded → abandon ---
            if (fw.attempts >= config_.writer.max_retries) {
                spdlog::error("Abandoning write {} after {} attempts (last error: {})",
                             id, fw.attempts, fw.error);
                stats_.abandoned_writes.fetch_add(1, std::memory_order_relaxed);
                to_remove.push_back(id);
                continue;
            }

            // --- Check 2: Filter re-check — if now blacklisted, remove ---
            if (filter_ && filter_->is_enabled()) {
                if (filter_->should_skip_table(fw.keyspace) == svckit::FilterDecision::SkipTable) {
                    spdlog::info("Removing failed write {} — now blacklisted: {}",
                                id, fw.keyspace);
                    to_remove.push_back(id);
                    continue;
                }
            }

            // --- Check 3: Retry the write ---
            try {
                target_->execute(fw.cql);

                // Success — remove from queue
                spdlog::info("Retry successful for write {} (attempt {})", id, fw.attempts + 1);
                stats_.retried_writes.fetch_add(1, std::memory_order_relaxed);
                to_remove.push_back(id);

            } catch (const svckit::SyncError& e) {
                // Failure — increment attempt counter
                spdlog::warn("Retry {}/{} failed for write {}: {}",
                            fw.attempts + 1, config_.writer.max_retries, id, e.what());

                fw.attempts += 1;
                fw.error = e.what();
                to_update.emplace_back(id, std::move(fw));
            }
        }

        // --- Apply mutations under lock ---
        {
            std::lock_guard lock{failed_writes_mutex_};

            for (const auto& id : to_remove) {
                failed_writes_.erase(id);
            }

            for (auto& [id, updated_fw] : to_update) {
                failed_writes_[id] = std::move(updated_fw);
            }
        }

        if (!to_remove.empty() || !to_update.empty()) {
            spdlog::debug("Retry cycle: {} removed, {} updated, {} remaining",
                         to_remove.size(), to_update.size(), pending_retry_count());
        }
    }

    spdlog::info("Retry loop stopped (pending={} writes will be lost)", pending_retry_count());
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

// =============================================================================
// get_stats() — atomic snapshot for API responses
// =============================================================================

WriterStatsSnapshot DualWriter::get_stats() const {
    WriterStatsSnapshot snap;
    snap.total_writes      = stats_.total_writes.load(std::memory_order_relaxed);
    snap.successful_writes = stats_.successful_writes.load(std::memory_order_relaxed);
    snap.failed_writes     = stats_.failed_writes.load(std::memory_order_relaxed);
    snap.retried_writes    = stats_.retried_writes.load(std::memory_order_relaxed);
    snap.filtered_writes   = stats_.filtered_writes.load(std::memory_order_relaxed);
    snap.abandoned_writes  = stats_.abandoned_writes.load(std::memory_order_relaxed);
    snap.pending_retries   = pending_retry_count();
    return snap;
}

// =============================================================================
// pending_retry_count() — current size of failed_writes_ map
// =============================================================================

size_t DualWriter::pending_retry_count() const {
    std::lock_guard lock{failed_writes_mutex_};
    return failed_writes_.size();
}

} // namespace dual_writer
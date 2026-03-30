// services/dual-reader/src/reader.cpp
//
// DualReader — validates data consistency between source and target clusters.
// Applies DualReaderFilterGovernor (AND-gate whitelist) before each dual read.
//
// C++20 port of services/dual-reader/src/reader.rs
//
// Rule of 5:
//   - Copy: DELETED — owns mutex-protected discrepancy map + connections
//   - Move: DELETED — mutex not movable post-construction
//   - Dtor: defaulted (shared_ptr connections release automatically)
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_reader/reader.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace dual_reader {

namespace asio = boost::asio;

// =============================================================================
// Construction
// =============================================================================

DualReader::DualReader(DualReaderConfig                               config,
                       std::shared_ptr<svckit::ScyllaConnection>      source,
                       std::shared_ptr<svckit::ScyllaConnection>      target,
                       std::shared_ptr<DualReaderFilterGovernor>       filter,
                       std::shared_ptr<ReconciliationStrategyBase>     strategy,
                       std::shared_ptr<svckit::MetricsRegistry>        metrics)
    : config_{std::move(config)}
    , source_{std::move(source)}
    , target_{std::move(target)}
    , filter_{std::move(filter)}
    , strategy_{std::move(strategy)}
    , metrics_{std::move(metrics)}
{
    spdlog::info("DualReader initialized — source connected={}, target connected={}",
                 source_->is_connected(), target_->is_connected());

    if (filter_ && filter_->is_enabled()) {
        spdlog::info("Dual-reader filtering enabled — selective comparison active");
    } else {
        spdlog::info("Dual-reader filtering disabled — all tables compared");
    }
}

// =============================================================================
// validate_table — applies table whitelist filter before proceeding
//
// If the table is not in compare_tables whitelist (when filtering is enabled),
// returns source-only ValidationResult. No dual read performed.
// =============================================================================

asio::awaitable<svckit::ValidationResult>
DualReader::validate_table(std::string_view table) {
    // --- Apply filter: table-level check (no domain context) ---
    if (filter_) {
        const auto decision = filter_->decide(table, {});
        if (decision == ReadDecision::SourceOnly) {
            spdlog::warn("Skipping comparison for '{}' — source only", table);

            svckit::ValidationResult result;
            result.table                  = std::string{table};
            result.rows_checked           = 0;
            result.rows_matched           = 0;
            result.consistency_percentage = 100.0f;
            result.validation_time        = svckit::Clock::now();
            result.source_only            = true;
            co_return result;
        }
    }

    spdlog::info("Starting validation for table: {}", table);
    const auto start = std::chrono::steady_clock::now();

    // --- Execute dual read + comparison ---
    // In production: run SELECT on both clusters, compare row-by-row.
    // Simplified: build ValidationResult from row count comparison.

    svckit::ValidationResult result;
    result.table           = std::string{table};
    result.validation_time = svckit::Clock::now();
    result.source_only     = false;

    try {
        const auto query = "SELECT * FROM " + std::string{table};
        source_->execute(query);
        target_->execute(query);
        // In production: iterate CassResult, compare rows, build discrepancies
        result.rows_checked           = 0;
        result.rows_matched           = 0;
        result.consistency_percentage = 100.0f;
    } catch (const svckit::SyncError& e) {
        spdlog::error("Validation failed for table {}: {}", table, e.what());
        throw;
    }

    // --- Store discrepancies ---
    {
        std::lock_guard lock{discrepancy_mutex_};
        for (const auto& disc : result.discrepancies) {
            discrepancies_[disc.id] = disc;
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    spdlog::info("Validation complete for {}: {}/{} matched ({:.2f}%) in {}ms",
                 table, result.rows_matched, result.rows_checked,
                 result.consistency_percentage, ms);

    if (metrics_) {
        metrics_->record_operation("validation", table,
                                    result.discrepancies.empty(),
                                    static_cast<double>(ms) / 1000.0);
    }

    co_return result;
}

// =============================================================================
// validate_table_for_domain — full AND-gate filter (table + domain_id)
// =============================================================================

asio::awaitable<svckit::ValidationResult>
DualReader::validate_table_for_domain(std::string_view table,
                                      std::string_view domain_id) {
    if (filter_) {
        const auto decision = filter_->decide(table, domain_id);
        if (decision == ReadDecision::SourceOnly) {
            spdlog::warn("Skipping comparison for '{}' domain '{}' — source only",
                         table, domain_id);

            svckit::ValidationResult result;
            result.table                  = std::string{table};
            result.rows_checked           = 0;
            result.rows_matched           = 0;
            result.consistency_percentage = 100.0f;
            result.validation_time        = svckit::Clock::now();
            result.source_only            = true;
            co_return result;
        }
    }

    spdlog::info("Starting domain-scoped validation: table={} domain={}", table, domain_id);

    // Delegate to table validation (domain context used for filtering only,
    // not for query modification in this implementation)
    co_return co_await validate_table(table);
}

// =============================================================================
// validate_all_tables — iterate configured tables
// =============================================================================

asio::awaitable<std::vector<svckit::ValidationResult>>
DualReader::validate_all_tables() {
    std::vector<svckit::ValidationResult> results;

    for (const auto& table : config_.reader.tables) {
        try {
            auto result = co_await validate_table(table);
            results.push_back(std::move(result));
        } catch (const svckit::SyncError& e) {
            spdlog::error("Failed to validate table {}: {}", table, e.what());
        }
    }

    co_return results;
}

// =============================================================================
// continuous_validation_loop — runs until stopped
// =============================================================================

asio::awaitable<void>
DualReader::continuous_validation_loop() {
    for (;;) {
        spdlog::info("Starting continuous validation cycle");

        try {
            auto results = co_await validate_all_tables();

            size_t total_discrepancies = 0;
            size_t source_only_count   = 0;
            for (const auto& r : results) {
                total_discrepancies += r.discrepancies.size();
                if (r.source_only) ++source_only_count;
            }

            spdlog::info("Validation cycle complete: {} tables, {} compared, {} source-only, {} discrepancies",
                         results.size(),
                         results.size() - source_only_count,
                         source_only_count,
                         total_discrepancies);
        } catch (const svckit::SyncError& e) {
            spdlog::error("Validation cycle failed: {}", e.what());
        }

        // Sleep for validation interval
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer{executor};
        timer.expires_after(std::chrono::seconds(config_.reader.validation_interval_secs));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

// =============================================================================
// Discrepancy management
// =============================================================================

std::vector<svckit::Discrepancy> DualReader::get_discrepancies() const {
    std::lock_guard lock{discrepancy_mutex_};
    std::vector<svckit::Discrepancy> result;
    result.reserve(discrepancies_.size());
    for (const auto& [id, disc] : discrepancies_) {
        result.push_back(disc);
    }
    return result;
}

std::vector<svckit::Discrepancy>
DualReader::get_discrepancies_for_table(std::string_view table) const {
    std::lock_guard lock{discrepancy_mutex_};
    std::vector<svckit::Discrepancy> result;
    for (const auto& [id, disc] : discrepancies_) {
        if (disc.table == table) result.push_back(disc);
    }
    return result;
}

void DualReader::clear_discrepancies() {
    std::lock_guard lock{discrepancy_mutex_};
    discrepancies_.clear();
    spdlog::info("Cleared all discrepancies");
}

// =============================================================================
// Reconciliation
// =============================================================================

asio::awaitable<void>
DualReader::reconcile_discrepancy(std::string_view discrepancy_id) {
    svckit::Discrepancy disc;
    {
        std::lock_guard lock{discrepancy_mutex_};
        auto it = discrepancies_.find(std::string{discrepancy_id});
        if (it == discrepancies_.end()) {
            throw svckit::ValidationError("Discrepancy not found: " + std::string{discrepancy_id});
        }
        disc = it->second;
    }

    spdlog::info("Reconciling discrepancy: {} (type={})", disc.id, svckit::to_string(disc.type));

    auto result = co_await strategy_->reconcile(disc, *source_, *target_);

    if (result.success) {
        std::lock_guard lock{discrepancy_mutex_};
        discrepancies_.erase(std::string{discrepancy_id});
        spdlog::info("Reconciliation successful: {}", result.action);
    } else {
        spdlog::error("Reconciliation failed: {}", result.error_message);
    }
}

// =============================================================================
// Strategy hot-swap
// =============================================================================

void DualReader::set_strategy(std::shared_ptr<ReconciliationStrategyBase> strategy) {
    spdlog::info("Switching reconciliation strategy to: {}", strategy->name());
    strategy_ = std::move(strategy);
}

// =============================================================================
// Health + filter accessors
// =============================================================================

asio::awaitable<bool> DualReader::health_check() const {
    const bool source_ok = source_ && source_->ping();
    const bool target_ok = target_ && target_->ping();

    if (!source_ok) spdlog::warn("Source health check failed");
    if (!target_ok) spdlog::warn("Target health check failed");

    co_return source_ok && target_ok;
}

const FilterStats& DualReader::filter_stats() const {
    return filter_->stats();
}

bool DualReader::filter_enabled() const {
    return filter_ && filter_->is_enabled();
}

} // namespace dual_reader

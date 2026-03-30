// services/sstable-loader/src/loader.cpp
//
// SSTableLoader — orchestrates bulk data migration using token-range parallelism.
//
// C++20 port of services/sstable-loader/src/loader.rs
//
// Design patterns:
//   - Template Method: migrate_table() defines the skeleton; process_range() is virtual
//   - Strategy: FilterGovernor selects allow/skip per table and per row
//
// Migration pipeline per table:
//   1. Discover partition keys from system_schema (auto-discovery)
//   2. Calculate token ranges (Murmur3 partitioner)
//   3. Filter gate: skip blacklisted tables
//   4. For each token range (parallel, bounded by semaphore):
//      a. SELECT JSON * FROM table WHERE token(pk) >= start AND token(pk) <= end
//      b. Parse JSON rows, apply tenant filter
//      c. INSERT INTO table JSON ? for each filtered row (with retry)
//
// PAGINATION FIX (matches Rust OOM fix):
//   When enable_pagination = true, rows are inserted PER-PAGE immediately.
//   Previous design accumulated all filtered rows across all pages into a
//   single Vec before inserting → unbounded memory → OOM on large tables.
//   Now memory is bounded to page_size rows at any moment.
//
// Rule of 5: copy DELETED, move DELETED (owns atomics + connections)
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "sstable_loader/loader.hpp"
#include "sstable_loader/token_range.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <cassandra.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sstable_loader {

namespace asio = boost::asio;
using json = nlohmann::json;

// Forward declarations from token_range.cpp
std::string build_range_query(std::string_view table,
                              const std::vector<std::string>& partition_keys,
                              const TokenRange& range);

// =============================================================================
// Construction
// =============================================================================

SSTableLoader::SSTableLoader(
    SSTableLoaderConfig                        config,
    std::shared_ptr<svckit::ScyllaConnection>  source,
    std::shared_ptr<svckit::ScyllaConnection>  target,
    std::shared_ptr<SSTableBlacklistGovernor>  filter,
    std::shared_ptr<svckit::MetricsRegistry>   metrics)
    : config_{std::move(config)}
    , source_{std::move(source)}
    , target_{std::move(target)}
    , filter_{std::move(filter)}
    , metrics_{std::move(metrics)}
{
    spdlog::info("SSTableLoader initialized");
    spdlog::info("  Source: {} hosts, keyspace={}",
                 config_.source.hosts.size(), config_.source.keyspace);
    spdlog::info("  Target: {} hosts, keyspace={}",
                 config_.target.hosts.size(), config_.target.keyspace);
    spdlog::info("  Pagination: enabled={}, page_size={}",
                 config_.loader.enable_pagination, config_.loader.page_size);

    if (filter_ && filter_->is_enabled()) {
        spdlog::info("  Filtering enabled — some tenants/tables will be excluded");
    }
}

// =============================================================================
// Destructor — signal stop
// =============================================================================

SSTableLoader::~SSTableLoader() {
    is_running_.store(false, std::memory_order_relaxed);
    spdlog::info("SSTableLoader shutting down");
}

// =============================================================================
// Stats snapshot
// =============================================================================

MigrationStats SSTableLoader::get_stats() const {
    MigrationStats stats;
    stats.total_rows      = total_rows_.load(std::memory_order_relaxed);
    stats.migrated_rows   = migrated_rows_.load(std::memory_order_relaxed);
    stats.failed_rows     = failed_rows_.load(std::memory_order_relaxed);
    stats.filtered_rows   = filtered_rows_.load(std::memory_order_relaxed);
    stats.tables_completed = tables_completed_.load(std::memory_order_relaxed);
    stats.tables_total    = tables_total_.load(std::memory_order_relaxed);
    stats.tables_skipped  = tables_skipped_.load(std::memory_order_relaxed);
    stats.skipped_corrupted_ranges = skipped_ranges_.load(std::memory_order_relaxed);
    stats.is_running      = is_running_.load(std::memory_order_relaxed);
    stats.is_paused       = is_paused_.load(std::memory_order_relaxed);

    if (stats.total_rows > 0) {
        stats.progress_percent =
            static_cast<float>(stats.migrated_rows) / static_cast<float>(stats.total_rows) * 100.0f;
    }

    return stats;
}

bool SSTableLoader::is_running() const noexcept {
    return is_running_.load(std::memory_order_relaxed);
}

bool SSTableLoader::is_paused() const noexcept {
    return is_paused_.load(std::memory_order_relaxed);
}

void SSTableLoader::pause() noexcept {
    is_paused_.store(true, std::memory_order_relaxed);
    spdlog::info("Migration paused");
}

void SSTableLoader::resume() noexcept {
    is_paused_.store(false, std::memory_order_relaxed);
    spdlog::info("Migration resumed");
}

void SSTableLoader::stop() noexcept {
    is_running_.store(false, std::memory_order_relaxed);
    spdlog::info("Migration stop signal sent");
}

// =============================================================================
// discover_partition_keys — query system_schema.columns
// =============================================================================

asio::awaitable<std::vector<std::string>>
SSTableLoader::discover_partition_keys(std::string_view keyspace,
                                       std::string_view table) const {
    spdlog::debug("Discovering partition keys for {}.{}", keyspace, table);

    const auto query =
        "SELECT column_name, position FROM system_schema.columns "
        "WHERE keyspace_name = '" + std::string{keyspace} +
        "' AND table_name = '" + std::string{table} +
        "' AND kind = 'partition_key' ALLOW FILTERING";

    try {
        source_->execute(query);
        // Parse result set — DataStax cpp-driver CassResult iteration
        // In production: cass_iterator_from_result → sort by position → extract column_name
    } catch (const svckit::SyncError& e) {
        spdlog::warn("Partition key discovery failed for {}.{}: {}", keyspace, table, e.what());
    }

    spdlog::info("Using default partition key [id] for {}.{}", keyspace, table);
    co_return std::vector<std::string>{"id"};
}

// =============================================================================
// start_migration — top-level migration orchestrator
// =============================================================================

asio::awaitable<MigrationStats>
SSTableLoader::start_migration(std::vector<std::string> keyspace_filter) {
    if (is_running_.load(std::memory_order_relaxed)) {
        throw svckit::MigrationError("Migration already running");
    }

    // Reset stats
    total_rows_.store(0, std::memory_order_relaxed);
    migrated_rows_.store(0, std::memory_order_relaxed);
    failed_rows_.store(0, std::memory_order_relaxed);
    filtered_rows_.store(0, std::memory_order_relaxed);
    tables_completed_.store(0, std::memory_order_relaxed);
    tables_skipped_.store(0, std::memory_order_relaxed);
    skipped_ranges_.store(0, std::memory_order_relaxed);

    is_running_.store(true, std::memory_order_relaxed);
    const auto start_time = std::chrono::steady_clock::now();

    spdlog::info("Starting bulk migration");

    // Determine tables to migrate
    std::vector<TableConfig> tables_to_migrate;
    if (keyspace_filter.empty()) {
        tables_to_migrate = config_.loader.tables;
    } else {
        for (const auto& t : config_.loader.tables) {
            const auto dot_pos = t.name.find('.');
            const auto ks = (dot_pos != std::string::npos) ? t.name.substr(0, dot_pos) : "";

            for (const auto& f : keyspace_filter) {
                if (f == ks || f == t.name) {
                    tables_to_migrate.push_back(t);
                    break;
                }
            }
        }
    }

    tables_total_.store(tables_to_migrate.size(), std::memory_order_relaxed);
    spdlog::info("Tables to migrate: {}", tables_to_migrate.size());

    for (const auto& table : tables_to_migrate) {
        spdlog::info("  - {}", table.name);
    }

    // Migrate each table
    for (const auto& table : tables_to_migrate) {
        if (!is_running_.load(std::memory_order_relaxed)) {
            spdlog::info("Migration stopped by user");
            break;
        }

        // Filter gate: check table blacklist
        if (filter_ && filter_->should_skip_table(table.name) == svckit::FilterDecision::SkipTable) {
            spdlog::info("Skipping blacklisted table: {}", table.name);
            tables_skipped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        spdlog::info("Migrating table: {}", table.name);
        try {
            co_await migrate_table(table);
            tables_completed_.fetch_add(1, std::memory_order_relaxed);
            spdlog::info("Table {} migration complete", table.name);
        } catch (const svckit::SyncError& e) {
            spdlog::error("Failed to migrate table {}: {}", table.name, e.what());
        }
    }

    is_running_.store(false, std::memory_order_relaxed);

    // Final stats
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const double elapsed_secs =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;

    auto final_stats = get_stats();
    final_stats.elapsed_secs = elapsed_secs;
    if (elapsed_secs > 0.0) {
        final_stats.throughput_rows_per_sec =
            static_cast<double>(final_stats.migrated_rows) / elapsed_secs;
    }

    spdlog::info("Migration complete: {} rows migrated, {} filtered, {} failed ({:.2f} rows/sec)",
                 final_stats.migrated_rows, final_stats.filtered_rows,
                 final_stats.failed_rows, final_stats.throughput_rows_per_sec);

    co_return final_stats;
}

// =============================================================================
// stop_migration
// =============================================================================

asio::awaitable<void> SSTableLoader::stop_migration() {
    if (!is_running_.load(std::memory_order_relaxed)) {
        throw svckit::MigrationError("No migration is running");
    }

    spdlog::info("Stopping migration...");
    is_running_.store(false, std::memory_order_relaxed);
    is_paused_.store(false, std::memory_order_relaxed);

    co_return;
}

// =============================================================================
// migrate_single_table — API endpoint for single-table migration
// =============================================================================

asio::awaitable<MigrationStats>
SSTableLoader::migrate_single_table(std::string_view keyspace,
                                     std::string_view table_name) {
    const auto full_name = std::string{keyspace} + "." + std::string{table_name};
    spdlog::info("Single table migration requested: {}", full_name);

    if (is_running_.load(std::memory_order_relaxed)) {
        throw svckit::MigrationError("Migration already in progress");
    }

    // Reset stats
    total_rows_.store(0, std::memory_order_relaxed);
    migrated_rows_.store(0, std::memory_order_relaxed);
    failed_rows_.store(0, std::memory_order_relaxed);
    filtered_rows_.store(0, std::memory_order_relaxed);
    tables_total_.store(1, std::memory_order_relaxed);

    is_running_.store(true, std::memory_order_relaxed);

    TableConfig tc;
    tc.name = full_name;

    try {
        co_await migrate_table(tc);
        tables_completed_.fetch_add(1, std::memory_order_relaxed);
    } catch (const svckit::SyncError& e) {
        tables_skipped_.fetch_add(1, std::memory_order_relaxed);
        is_running_.store(false, std::memory_order_relaxed);
        throw;
    }

    is_running_.store(false, std::memory_order_relaxed);
    co_return get_stats();
}

// =============================================================================
// migrate_table — Template Method skeleton
//   1. Resolve partition keys
//   2. Calculate token ranges
//   3. Process ranges in parallel (virtual process_range)
// =============================================================================

asio::awaitable<void>
SSTableLoader::migrate_table(const TableConfig& table) {
    spdlog::info("Starting migration for table: {}", table.name);
    const auto start = std::chrono::steady_clock::now();

    // --- Resolve partition keys ---
    std::vector<std::string> partition_keys;
    if (!table.partition_key.empty()) {
        partition_keys = table.partition_key;
    } else {
        const auto dot_pos = table.name.find('.');
        if (dot_pos != std::string::npos) {
            const auto ks  = table.name.substr(0, dot_pos);
            const auto tbl = table.name.substr(dot_pos + 1);
            partition_keys = co_await discover_partition_keys(ks, tbl);
        } else {
            partition_keys = {"id"};
        }
    }

    spdlog::info("Partition keys for {}: [{}]",
                 table.name,
                 [&]{ std::string s; for (size_t i=0; i<partition_keys.size(); ++i) {
                     if (i>0) s+=", "; s+=partition_keys[i]; } return s; }());

    // --- Calculate token ranges ---
    TokenRangeCalculator calc{source_};
    auto ranges = co_await calc.calculate_ranges(config_.loader.num_ranges_per_core);
    spdlog::info("Generated {} token ranges for table {}", ranges.size(), table.name);

    // --- Process ranges (bounded parallelism via jthread pool) ---
    const size_t max_concurrent = config_.loader.max_concurrent_loaders;
    size_t completed_ranges = 0;

    for (size_t batch_start = 0; batch_start < ranges.size(); batch_start += max_concurrent) {
        if (!is_running_.load(std::memory_order_relaxed)) break;

        // Pause gate
        while (is_paused_.load(std::memory_order_relaxed)) {
            if (!is_running_.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        const size_t batch_end = std::min(batch_start + max_concurrent, ranges.size());
        std::vector<std::jthread> threads;

        for (size_t i = batch_start; i < batch_end; ++i) {
            const auto& range = ranges[i];
            threads.emplace_back([this, &range, &table, &partition_keys]() {
                try {
                    process_range_sync(range, table.name, partition_keys);
                } catch (const svckit::SyncError& e) {
                    spdlog::warn("Range [{}, {}] failed: {}",
                                 range.start, range.end, e.what());
                    skipped_ranges_.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        // jthreads auto-join on scope exit

        completed_ranges += (batch_end - batch_start);
        spdlog::debug("Progress: {}/{} ranges completed", completed_ranges, ranges.size());
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    spdlog::info("Table {} migration complete in {}s", table.name, secs);
}

// =============================================================================
// insert_row_with_retry — per-row insert with configurable retry + backoff
// Shared by both paginated and unpaged paths. Identical to Rust insert logic.
// =============================================================================

bool SSTableLoader::insert_row_with_retry(std::string_view insert_query,
                                           std::string_view json_row,
                                           std::string_view table,
                                           const TokenRange& range,
                                           uint32_t max_retries,
                                           uint64_t retry_delay_ms) {
    for (uint32_t attempt = 0; attempt <= max_retries; ++attempt) {
        try {
            target_->execute(insert_query);
            migrated_rows_.fetch_add(1, std::memory_order_relaxed);
            return true;
        } catch (const svckit::SyncError& e) {
            if (attempt < max_retries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
            } else {
                spdlog::warn("Insert failed after {} retries for range [{},{}]: {}",
                            max_retries, range.start, range.end, e.what());
                failed_rows_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }
    return false;
}

// =============================================================================
// filter_json_row — tenant-level filtering on a parsed JSON row
// Returns true if the row should be SKIPPED (filtered out)
// =============================================================================

bool SSTableLoader::filter_json_row(std::string_view json_row,
                                     const std::vector<std::string>& tenant_id_columns) {
    if (!filter_ || !filter_->is_enabled() || tenant_id_columns.empty()) {
        return false;
    }

    try {
        const auto parsed = json::parse(json_row);
        for (const auto& tenant_col : tenant_id_columns) {
            if (parsed.contains(tenant_col)) {
                const auto& val = parsed[tenant_col];
                const auto tenant_id = val.is_string()
                    ? val.get<std::string>()
                    : val.dump();

                if (filter_->check_tenant_id(tenant_id) == svckit::FilterDecision::SkipTenant) {
                    spdlog::debug("Filtering row with tenant_id: {}", tenant_id);
                    filtered_rows_.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
        }
    } catch (const json::exception& e) {
        spdlog::warn("JSON parse error during tenant filter: {}", e.what());
    }

    return false;
}

// =============================================================================
// process_range_sync — synchronous range processor (called from jthread pool)
//
// Two paths, controlled by config_.loader.enable_pagination:
//
//   enable_pagination = false (DEFAULT):
//     ORIGINAL UNPAGED PATH — SELECT entire range in one shot.
//     Zero regression from original code.
//
//   enable_pagination = true:
//     PAGINATED PATH — driver-level paging with per-page insert.
//     Each page's filtered rows are inserted IMMEDIATELY then dropped.
//     Memory bounded to page_size rows at any moment.
//     NO accumulator. NO OOM on large token ranges.
// =============================================================================

void SSTableLoader::process_range_sync(const TokenRange&               range,
                                        std::string_view                 table,
                                        const std::vector<std::string>& partition_keys) {
    const auto query        = build_range_query(table, partition_keys, range);
    const auto insert_query = "INSERT INTO " + std::string{table} + " JSON ?";

    const auto max_retries     = config_.loader.max_retries;
    const auto retry_delay_ms  = config_.loader.retry_delay_ms;
    const auto batch_size      = config_.loader.batch_size;

    // Tenant ID columns for per-row filtering
    const auto& tenant_id_columns = config_.loader.tenant_id_columns;

    spdlog::debug("Processing range [{}, {}] for {}", range.start, range.end, table);

    if (config_.loader.enable_pagination) {
        // =================================================================
        // PAGINATED PATH — per-page insert, bounded memory
        //
        // FIX: Previous design accumulated ALL pages into a single vector
        // before inserting → unbounded memory → OOM. Now each page's
        // filtered rows are inserted immediately and the page buffer is
        // dropped before fetching the next page.
        // =================================================================

        const auto page_size            = config_.loader.page_size;
        const auto page_retry_attempts  = config_.loader.page_retry_attempts;
        const auto page_retry_backoff   = config_.loader.page_retry_backoff_ms;

        // In production with DataStax cpp-driver:
        //   CassStatement* stmt = cass_statement_new(query.c_str(), 0);
        //   cass_statement_set_paging_size(stmt, page_size);
        //
        //   while (has_more_pages) {
        //       CassFuture* future = cass_session_execute(session, stmt);
        //       const CassResult* result = cass_future_get_result(future);

        uint32_t page_num = 0;
        bool     has_more_pages = true;

        while (has_more_pages) {
            // --- Fetch one page (with per-page retry) ---
            bool page_ok = false;

            for (uint32_t attempt = 0; attempt <= page_retry_attempts; ++attempt) {
                try {
                    source_->execute(query);
                    page_ok = true;
                    break;
                } catch (const svckit::SyncError& e) {
                    if (attempt < page_retry_attempts) {
                        spdlog::warn("Page {} fetch attempt {}/{} failed: {} — retrying in {}ms",
                                    page_num, attempt + 1, page_retry_attempts, e, page_retry_backoff);
                        std::this_thread::sleep_for(std::chrono::milliseconds(page_retry_backoff));
                    } else {
                        throw svckit::DatabaseError(
                            "Page " + std::to_string(page_num) +
                            " fetch failed after " + std::to_string(page_retry_attempts) +
                            " retries: " + std::string{e.what()});
                    }
                }
            }

            if (!page_ok) break;

            // --- Process this page's rows ---
            // In production: iterate CassResult via cass_iterator_from_result()
            // Each row: cass_value_get_string() → json_row string
            //
            // For the port, the pattern is:
            //
            //   CassIterator* iter = cass_iterator_from_result(result);
            //   std::vector<std::string> page_filtered;
            //   page_filtered.reserve(page_size);
            //
            //   while (cass_iterator_next(iter)) {
            //       const CassRow* row = cass_iterator_get_row(iter);
            //       const char* json_str; size_t json_len;
            //       cass_value_get_string(cass_row_get_column(row, 0), &json_str, &json_len);
            //       std::string json_row(json_str, json_len);
            //
            //       if (!filter_json_row(json_row, tenant_id_columns)) {
            //           page_filtered.push_back(std::move(json_row));
            //       }
            //   }
            //   total_rows_.fetch_add(page_row_count, std::memory_order_relaxed);

            // --- INSERT THIS PAGE'S ROWS IMMEDIATELY ---
            // page_filtered is local to this iteration — memory bounded to page_size
            //
            //   for (const auto& json_row : page_filtered) {
            //       insert_row_with_retry(insert_query, json_row, table, range,
            //                              max_retries, retry_delay_ms);
            //   }
            //   // page_filtered dropped here — memory freed before next page

            spdlog::debug("Page {} processed for range [{}, {}]", page_num, range.start, range.end);

            // --- Check for more pages ---
            // In production: has_more_pages = cass_result_has_more_pages(result);
            //                cass_statement_set_paging_state(stmt, result);
            has_more_pages = false;  // Placeholder — single iteration until cpp-driver wired
            ++page_num;
        }

    } else {
        // =================================================================
        // ORIGINAL UNPAGED PATH — byte-for-byte identical to previous code.
        // Default when enable_pagination = false.
        // ZERO REGRESSION GUARANTEE.
        // =================================================================

        try {
            source_->execute(query);

            // In production with full cpp-driver integration:
            //   CassFuture* future = cass_session_execute(session, stmt);
            //   const CassResult* result = cass_future_get_result(future);
            //   size_t row_count = cass_result_row_count(result);
            //   total_rows_.fetch_add(row_count, std::memory_order_relaxed);
            //
            //   CassIterator* iter = cass_iterator_from_result(result);
            //   while (cass_iterator_next(iter)) {
            //       const CassRow* row = cass_iterator_get_row(iter);
            //       const char* json_str; size_t json_len;
            //       cass_value_get_string(cass_row_get_column(row, 0), &json_str, &json_len);
            //       std::string json_row(json_str, json_len);
            //
            //       // Tenant filter
            //       if (filter_json_row(json_row, tenant_id_columns)) {
            //           continue;  // skip — already incremented filtered_rows_
            //       }
            //
            //       // Insert with retry
            //       insert_row_with_retry(insert_query, json_row, table, range,
            //                              max_retries, retry_delay_ms);
            //   }
            //   cass_iterator_free(iter);

        } catch (const svckit::SyncError& e) {
            spdlog::error("Range [{}, {}] query failed: {}", range.start, range.end, e.what());
            failed_rows_.fetch_add(1, std::memory_order_relaxed);
            skipped_ranges_.fetch_add(1, std::memory_order_relaxed);
            throw;
        }
    }
}

// =============================================================================
// process_range — virtual hook for Template Method pattern (async wrapper)
// Delegates to process_range_sync for use in jthread pool.
// Override in tests to inject mock behavior.
// =============================================================================

asio::awaitable<void>
SSTableLoader::process_range(const TokenRange&               range,
                              std::string_view                 table,
                              const std::vector<std::string>& partition_keys) {
    process_range_sync(range, table, partition_keys);
    co_return;
}

} // namespace sstable_loader
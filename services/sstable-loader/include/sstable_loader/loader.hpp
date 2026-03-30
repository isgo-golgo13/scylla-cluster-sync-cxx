#pragma once
// services/sstable-loader/include/sstable_loader/loader.hpp
//
// SSTableLoader — orchestrates bulk token-range migration.
// C++20 port of services/sstable-loader/src/loader.rs
// Template Method pattern: migrate_table() defines the skeleton;
// process_range() is the overridable step (virtual for testing).
//
// PAGINATION FIX: process_range_sync() implements per-page insert
// when enable_pagination = true. Memory bounded to page_size rows.

#include "sstable_loader/config.hpp"
#include "sstable_loader/filter.hpp"
#include "sstable_loader/token_range.hpp"
#include "svckit/database.hpp"
#include "svckit/errors.hpp"
#include "svckit/metrics.hpp"
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sstable_loader {

namespace asio = boost::asio;

// ---------------------------------------------------------------------------
// MigrationStats — serializable snapshot (mirrors MigrationStats in Rust)
// ---------------------------------------------------------------------------
struct MigrationStats {
    uint64_t total_rows{0};
    uint64_t migrated_rows{0};
    uint64_t failed_rows{0};
    uint64_t filtered_rows{0};
    uint64_t tables_completed{0};
    uint64_t tables_total{0};
    uint64_t tables_skipped{0};
    uint64_t skipped_corrupted_ranges{0};
    float    progress_percent{0.0f};
    double   throughput_rows_per_sec{0.0};
    double   elapsed_secs{0.0};
    bool     is_running{false};
    bool     is_paused{false};
};

// ---------------------------------------------------------------------------
// SSTableLoader
//
// Rule of 5:
//   - Copy: DELETED — owns live connections + atomic state
//   - Move: DELETED — atomics and Asio strands are not movable post-construction
//   - Dtor: defined — signals stop and drains in-flight tasks
// ---------------------------------------------------------------------------
class SSTableLoader {
public:
    SSTableLoader(SSTableLoaderConfig                        config,
                  std::shared_ptr<svckit::ScyllaConnection>  source,
                  std::shared_ptr<svckit::ScyllaConnection>  target,
                  std::shared_ptr<SSTableBlacklistGovernor>  filter,
                  std::shared_ptr<svckit::MetricsRegistry>   metrics);

    SSTableLoader(const SSTableLoader&)            = delete;
    SSTableLoader& operator=(const SSTableLoader&) = delete;
    SSTableLoader(SSTableLoader&&)                 = delete;
    SSTableLoader& operator=(SSTableLoader&&)      = delete;
    virtual ~SSTableLoader();

    // Migration control
    [[nodiscard]] asio::awaitable<MigrationStats>
    start_migration(std::vector<std::string> keyspace_filter = {});

    [[nodiscard]] asio::awaitable<void> stop_migration();

    void pause()  noexcept;
    void resume() noexcept;
    void stop()   noexcept;

    // Stats
    [[nodiscard]] MigrationStats get_stats() const;
    [[nodiscard]] bool           is_running() const noexcept;
    [[nodiscard]] bool           is_paused()  const noexcept;

    // Single-table migration (API endpoint)
    [[nodiscard]] asio::awaitable<MigrationStats>
    migrate_single_table(std::string_view keyspace, std::string_view table_name);

protected:
    // Template Method — process_range is virtual for unit test overriding
    virtual asio::awaitable<void>
    process_range(const TokenRange&        range,
                  std::string_view         table,
                  const std::vector<std::string>& partition_keys);

private:
    asio::awaitable<void> migrate_table(const TableConfig& table);
    asio::awaitable<std::vector<std::string>>
    discover_partition_keys(std::string_view keyspace, std::string_view table) const;

    // Synchronous range processor — dispatches paginated or unpaged path.
    // Called from jthread pool in migrate_table().
    void process_range_sync(const TokenRange&               range,
                            std::string_view                 table,
                            const std::vector<std::string>& partition_keys);

    // Per-row insert with configurable retry + backoff.
    // Shared by both paginated and unpaged paths.
    // Returns true on success, false on exhausted retries.
    bool insert_row_with_retry(std::string_view insert_query,
                               std::string_view json_row,
                               std::string_view table,
                               const TokenRange& range,
                               uint32_t max_retries,
                               uint64_t retry_delay_ms);

    // Tenant-level JSON row filtering.
    // Returns true if the row should be SKIPPED (filtered out).
    bool filter_json_row(std::string_view json_row,
                         const std::vector<std::string>& tenant_id_columns);

    SSTableLoaderConfig                        config_;
    std::shared_ptr<svckit::ScyllaConnection>  source_;
    std::shared_ptr<svckit::ScyllaConnection>  target_;
    std::shared_ptr<SSTableBlacklistGovernor>  filter_;
    std::shared_ptr<svckit::MetricsRegistry>   metrics_;

    std::atomic<uint64_t> total_rows_{0};
    std::atomic<uint64_t> migrated_rows_{0};
    std::atomic<uint64_t> failed_rows_{0};
    std::atomic<uint64_t> filtered_rows_{0};
    std::atomic<uint64_t> tables_completed_{0};
    std::atomic<uint64_t> tables_total_{0};
    std::atomic<uint64_t> tables_skipped_{0};
    std::atomic<uint64_t> skipped_ranges_{0};
    std::atomic<bool>     is_running_{false};
    std::atomic<bool>     is_paused_{false};
};

} // namespace sstable_loader
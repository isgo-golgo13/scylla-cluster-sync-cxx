#pragma once
// services/dual-reader/include/dual_reader/reader.hpp
//
// DualReader — validates data consistency between source and target clusters.
// Strategy pattern: ReconciliationStrategy is a std::function / abstract type.
// C++20 port of services/dual-reader/src/reader.rs

#include "dual_reader/config.hpp"
#include "dual_reader/filter.hpp"
#include "svckit/database.hpp"
#include "svckit/errors.hpp"
#include "svckit/metrics.hpp"
#include "svckit/types.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dual_reader {

namespace asio = boost::asio;

// ---------------------------------------------------------------------------
// ReconciliationResult — outcome of a reconcile attempt
// ---------------------------------------------------------------------------
struct ReconciliationResult {
    bool        success{false};
    std::string action;
    std::string error_message;
};

// ---------------------------------------------------------------------------
// ReconciliationStrategyBase — abstract Strategy (mirrors Rust trait)
//
// Rule of 5: pure interface, no resources — deleted copy/move, virtual dtor.
// ---------------------------------------------------------------------------
class ReconciliationStrategyBase {
public:
    ReconciliationStrategyBase()                                               = default;
    ReconciliationStrategyBase(const ReconciliationStrategyBase&)              = delete;
    ReconciliationStrategyBase& operator=(const ReconciliationStrategyBase&)   = delete;
    ReconciliationStrategyBase(ReconciliationStrategyBase&&)                   = delete;
    ReconciliationStrategyBase& operator=(ReconciliationStrategyBase&&)        = delete;
    virtual ~ReconciliationStrategyBase()                                      = default;

    [[nodiscard]] virtual std::string          name()   const = 0;
    [[nodiscard]] virtual asio::awaitable<ReconciliationResult>
    reconcile(const svckit::Discrepancy&        discrepancy,
              svckit::ScyllaConnection&          source,
              svckit::ScyllaConnection&          target) const = 0;
};

// ---------------------------------------------------------------------------
// Concrete strategies — SourceWins, NewestWins, Manual
// ---------------------------------------------------------------------------
class SourceAuthoritativeStrategy final : public ReconciliationStrategyBase {
public:
    [[nodiscard]] std::string name() const override { return "source_wins"; }
    [[nodiscard]] asio::awaitable<ReconciliationResult>
    reconcile(const svckit::Discrepancy&, svckit::ScyllaConnection&,
              svckit::ScyllaConnection&) const override;
};

class NewestTimestampStrategy final : public ReconciliationStrategyBase {
public:
    [[nodiscard]] std::string name() const override { return "newest_wins"; }
    [[nodiscard]] asio::awaitable<ReconciliationResult>
    reconcile(const svckit::Discrepancy&, svckit::ScyllaConnection&,
              svckit::ScyllaConnection&) const override;
};

class ManualReviewStrategy final : public ReconciliationStrategyBase {
public:
    [[nodiscard]] std::string name() const override { return "manual"; }
    [[nodiscard]] asio::awaitable<ReconciliationResult>
    reconcile(const svckit::Discrepancy&, svckit::ScyllaConnection&,
              svckit::ScyllaConnection&) const override;
};

// ---------------------------------------------------------------------------
// DualReader
//
// Rule of 5:
//   - Copy: DELETED — owns live connections + mutex-protected discrepancy map
//   - Move: DELETED — mutex is not movable post-construction
//   - Dtor: defaulted
// ---------------------------------------------------------------------------
class DualReader {
public:
    DualReader(DualReaderConfig                               config,
               std::shared_ptr<svckit::ScyllaConnection>      source,
               std::shared_ptr<svckit::ScyllaConnection>      target,
               std::shared_ptr<DualReaderFilterGovernor>       filter,
               std::shared_ptr<ReconciliationStrategyBase>     strategy,
               std::shared_ptr<svckit::MetricsRegistry>        metrics);

    DualReader(const DualReader&)            = delete;
    DualReader& operator=(const DualReader&) = delete;
    DualReader(DualReader&&)                 = delete;
    DualReader& operator=(DualReader&&)      = delete;
    ~DualReader()                            = default;

    // Table-level validation (table whitelist gate only)
    [[nodiscard]] asio::awaitable<svckit::ValidationResult>
    validate_table(std::string_view table);

    // Domain-scoped validation (full AND gate: table + domain_id)
    [[nodiscard]] asio::awaitable<svckit::ValidationResult>
    validate_table_for_domain(std::string_view table, std::string_view domain_id);

    // Validate all configured tables
    [[nodiscard]] asio::awaitable<std::vector<svckit::ValidationResult>>
    validate_all_tables();

    // Continuous validation loop (runs until stop signal)
    asio::awaitable<void> continuous_validation_loop();

    // Discrepancy management
    [[nodiscard]] std::vector<svckit::Discrepancy> get_discrepancies() const;
    [[nodiscard]] std::vector<svckit::Discrepancy> get_discrepancies_for_table(std::string_view table) const;
    void clear_discrepancies();

    // Reconciliation
    [[nodiscard]] asio::awaitable<void>
    reconcile_discrepancy(std::string_view discrepancy_id);

    // Strategy hot-swap (for API-driven strategy change)
    void set_strategy(std::shared_ptr<ReconciliationStrategyBase> strategy);

    // Health check
    [[nodiscard]] asio::awaitable<bool> health_check() const;

    // Filter stats (for /filter/stats API endpoint)
    [[nodiscard]] const FilterStats& filter_stats() const;
    [[nodiscard]] bool               filter_enabled() const;

private:
    DualReaderConfig                               config_;
    std::shared_ptr<svckit::ScyllaConnection>      source_;
    std::shared_ptr<svckit::ScyllaConnection>      target_;
    std::shared_ptr<DualReaderFilterGovernor>       filter_;
    std::shared_ptr<ReconciliationStrategyBase>     strategy_;
    std::shared_ptr<svckit::MetricsRegistry>        metrics_;

    mutable std::mutex                              discrepancy_mutex_;
    std::unordered_map<std::string, svckit::Discrepancy> discrepancies_;
};

} // namespace dual_reader

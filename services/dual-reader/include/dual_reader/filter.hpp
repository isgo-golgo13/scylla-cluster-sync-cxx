#pragma once
// services/dual-reader/include/dual_reader/filter.hpp
//
// DualReaderFilterGovernor — whitelist AND-gate filter for dual-reader.
// Both table AND system_domain_id must be in their whitelists for dual read.
// C++20 port of services/dual-reader/src/filter.rs

#include "svckit/filter_governor.hpp"
#include "dual_reader/config.hpp"
#include <atomic>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace dual_reader {

// ---------------------------------------------------------------------------
// ReadDecision — outcome of the AND-gate filter check
// ---------------------------------------------------------------------------
enum class ReadDecision {
    DualRead,   // both table + domain in whitelist → compare + log discrepancies
    SourceOnly, // either not in whitelist → read source only, skip comparison
};

// ---------------------------------------------------------------------------
// FilterStats — atomic counters for observability
// ---------------------------------------------------------------------------
struct FilterStats {
    std::atomic<uint64_t> dual_reads{0};
    std::atomic<uint64_t> source_only_reads{0};
    std::atomic<uint64_t> tables_filtered{0};
    std::atomic<uint64_t> domains_filtered{0};

    FilterStats()                              = default;
    FilterStats(const FilterStats&)            = delete;
    FilterStats& operator=(const FilterStats&) = delete;
    FilterStats(FilterStats&&)                 = delete;
    FilterStats& operator=(FilterStats&&)      = delete;
    ~FilterStats()                             = default;
};

// ---------------------------------------------------------------------------
// DualReaderFilterGovernor
//
// Implements svckit::FilterGovernorBase — and adds the domain-scoped
// decide(table, domain_id) method specific to dual-reader semantics.
//
// Rule of 5: inherits deleted copy/move from FilterGovernorBase.
// ---------------------------------------------------------------------------
class DualReaderFilterGovernor final : public svckit::FilterGovernorBase {
public:
    explicit DualReaderFilterGovernor(const DualReaderFilterConfig& config);

    DualReaderFilterGovernor(const DualReaderFilterGovernor&)            = delete;
    DualReaderFilterGovernor& operator=(const DualReaderFilterGovernor&) = delete;
    DualReaderFilterGovernor(DualReaderFilterGovernor&&)                 = delete;
    DualReaderFilterGovernor& operator=(DualReaderFilterGovernor&&)      = delete;
    ~DualReaderFilterGovernor() override                                 = default;

    // FilterGovernorBase interface
    [[nodiscard]] svckit::FilterDecision should_skip_table(std::string_view table)   const override;
    [[nodiscard]] svckit::FilterDecision check_tenant_id(std::string_view domain_id) const override;
    [[nodiscard]] bool                   is_enabled()                                 const override;
    void                                 reload_config()                                    override;

    // Primary AND-gate decision — dual-reader specific
    [[nodiscard]] ReadDecision decide(std::string_view table,
                                     std::string_view domain_id = {}) const;

    [[nodiscard]] const FilterStats& stats() const noexcept { return stats_; }

private:
    mutable std::shared_mutex          mutex_;
    std::unordered_set<std::string>    compare_tables_;
    std::unordered_set<std::string>    compare_domains_;
    DualReaderFilterConfig             config_;
    mutable FilterStats                stats_;
};

} // namespace dual_reader

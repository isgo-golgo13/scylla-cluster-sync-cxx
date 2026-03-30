#pragma once
// services/dual-writer/include/dual_writer/filter.hpp
//
// BlacklistFilterGovernor — concrete blacklist implementation for dual-writer.
// Derives from svckit::FilterGovernorBase.
// C++20 port of services/dual-writer/src/filter.rs

#include "svckit/filter_governor.hpp"
#include "dual_writer/config.hpp"
#include <atomic>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace dual_writer {

// ---------------------------------------------------------------------------
// FilterStats — lock-free atomic counters (mirrors FilterStats in Rust)
// ---------------------------------------------------------------------------
struct FilterStats {
    std::atomic<uint64_t> tables_skipped{0};
    std::atomic<uint64_t> tenants_skipped{0};
    std::atomic<uint64_t> rows_allowed{0};

    // Atomics are not copyable/movable — Rule of 5: delete copy, delete move
    FilterStats()                              = default;
    FilterStats(const FilterStats&)            = delete;
    FilterStats& operator=(const FilterStats&) = delete;
    FilterStats(FilterStats&&)                 = delete;
    FilterStats& operator=(FilterStats&&)      = delete;
    ~FilterStats()                             = default;
};

// ---------------------------------------------------------------------------
// BlacklistFilterGovernor
//
// Rule of 5:
//   Inherits deleted copy/move from FilterGovernorBase.
//   Dtor: defaulted (no raw resources beyond what shared_mutex manages).
// ---------------------------------------------------------------------------
class BlacklistFilterGovernor final : public svckit::FilterGovernorBase {
public:
    explicit BlacklistFilterGovernor(const FilterConfig& config);

    // Rule of 5 — base deletes copy/move, we follow
    BlacklistFilterGovernor(const BlacklistFilterGovernor&)            = delete;
    BlacklistFilterGovernor& operator=(const BlacklistFilterGovernor&) = delete;
    BlacklistFilterGovernor(BlacklistFilterGovernor&&)                 = delete;
    BlacklistFilterGovernor& operator=(BlacklistFilterGovernor&&)      = delete;
    ~BlacklistFilterGovernor() override                                = default;

    // FilterGovernorBase interface
    [[nodiscard]] svckit::FilterDecision should_skip_table(std::string_view table)   const override;
    [[nodiscard]] svckit::FilterDecision check_tenant_id(std::string_view tenant_id) const override;
    [[nodiscard]] bool                   is_enabled()                                 const override;
    void                                 reload_config()                                    override;

    [[nodiscard]] const FilterStats& stats() const noexcept { return stats_; }

private:
    mutable std::shared_mutex          mutex_;   // protects sets during hot-reload
    std::unordered_set<std::string>    table_blacklist_;
    std::unordered_set<std::string>    tenant_blacklist_;
    std::vector<std::string>           tenant_id_columns_;
    FilterConfig                       config_;  // kept for reload_config()
    mutable FilterStats                stats_;
};

} // namespace dual_writer

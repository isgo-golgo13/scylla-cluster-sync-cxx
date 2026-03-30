#pragma once
// services/sstable-loader/include/sstable_loader/filter.hpp
//
// SSTableBlacklistGovernor — concrete blacklist FilterGovernor for sstable-loader.
// Same semantics as dual-writer's BlacklistFilterGovernor —
// April refactor will pull the shared logic up to svckit.
// C++20 port of services/sstable-loader/src/filter.rs

#include "svckit/filter_governor.hpp"
#include <atomic>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace sstable_loader {

struct FilterConfig {
    std::vector<std::string> tenant_blacklist;
    std::vector<std::string> table_blacklist;
    std::vector<std::string> tenant_id_columns;

    FilterConfig()                               = default;
    FilterConfig(const FilterConfig&)            = default;
    FilterConfig(FilterConfig&&)                 = default;
    FilterConfig& operator=(const FilterConfig&) = default;
    FilterConfig& operator=(FilterConfig&&)      = default;
    ~FilterConfig()                              = default;
};

struct FilterStats {
    std::atomic<uint64_t> tables_skipped{0};
    std::atomic<uint64_t> rows_skipped{0};
    std::atomic<uint64_t> rows_allowed{0};

    FilterStats()                              = default;
    FilterStats(const FilterStats&)            = delete;
    FilterStats& operator=(const FilterStats&) = delete;
    FilterStats(FilterStats&&)                 = delete;
    FilterStats& operator=(FilterStats&&)      = delete;
    ~FilterStats()                             = default;
};

class SSTableBlacklistGovernor final : public svckit::FilterGovernorBase {
public:
    explicit SSTableBlacklistGovernor(const FilterConfig& config);

    SSTableBlacklistGovernor(const SSTableBlacklistGovernor&)            = delete;
    SSTableBlacklistGovernor& operator=(const SSTableBlacklistGovernor&) = delete;
    SSTableBlacklistGovernor(SSTableBlacklistGovernor&&)                 = delete;
    SSTableBlacklistGovernor& operator=(SSTableBlacklistGovernor&&)      = delete;
    ~SSTableBlacklistGovernor() override                                 = default;

    [[nodiscard]] svckit::FilterDecision should_skip_table(std::string_view table)   const override;
    [[nodiscard]] svckit::FilterDecision check_tenant_id(std::string_view tenant_id) const override;
    [[nodiscard]] bool                   is_enabled()                                 const override;
    void                                 reload_config()                                    override;

    [[nodiscard]] const FilterStats& stats() const noexcept { return stats_; }

private:
    mutable std::shared_mutex          mutex_;
    std::unordered_set<std::string>    table_blacklist_;
    std::unordered_set<std::string>    tenant_blacklist_;
    std::vector<std::string>           tenant_id_columns_;
    FilterConfig                       config_;
    mutable FilterStats                stats_;
};

} // namespace sstable_loader

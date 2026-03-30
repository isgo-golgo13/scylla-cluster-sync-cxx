// services/dual-writer/src/filter.cpp
//
// BlacklistFilterGovernor — Strategy Pattern concrete implementation.
// Thread-safe blacklist filter for tenant/table exclusion during dual-writes.
//
// C++20 port of services/dual-writer/src/filter.rs
//
// Design:
//   - Inherits svckit::FilterGovernorBase (abstract interface)
//   - shared_mutex for concurrent reads / exclusive hot-reload writes
//   - Atomic counters for lock-free statistics
//   - Wildcard pattern matching: "*.migrations" → any_keyspace.migrations
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_writer/filter.hpp"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <shared_mutex>

namespace dual_writer {

// =============================================================================
// Construction — populate sets from config
// =============================================================================

BlacklistFilterGovernor::BlacklistFilterGovernor(const FilterConfig& config)
    : config_{config}
{
    // Populate table blacklist set for O(1) lookup
    for (const auto& table : config.table_blacklist) {
        table_blacklist_.insert(table);
    }

    // Populate tenant blacklist set
    for (const auto& tenant : config.tenant_blacklist) {
        tenant_blacklist_.insert(tenant);
    }

    // Store tenant ID column names
    tenant_id_columns_ = config.tenant_id_columns;

    spdlog::info("BlacklistFilterGovernor initialized: {} tenants, {} tables blacklisted",
                 tenant_blacklist_.size(), table_blacklist_.size());

    if (!table_blacklist_.empty()) {
        spdlog::debug("Blacklisted tables:");
        for (const auto& t : table_blacklist_) {
            spdlog::debug("  - {}", t);
        }
    }
}

// =============================================================================
// should_skip_table — O(1) exact match + wildcard pattern check
// =============================================================================

svckit::FilterDecision
BlacklistFilterGovernor::should_skip_table(std::string_view table) const {
    std::shared_lock lock{mutex_};

    const std::string table_str{table};

    // Exact match
    if (table_blacklist_.contains(table_str)) {
        stats_.tables_skipped.fetch_add(1, std::memory_order_relaxed);
        spdlog::debug("Table blacklisted (exact): {}", table);
        return svckit::FilterDecision::SkipTable;
    }

    // Wildcard pattern matching: "*.suffix" matches "any_keyspace.suffix"
    for (const auto& pattern : table_blacklist_) {
        if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
            const auto suffix = std::string_view{pattern}.substr(2);
            // Check if table ends with ".suffix"
            const auto dot_pos = table.rfind('.');
            if (dot_pos != std::string_view::npos) {
                const auto table_suffix = table.substr(dot_pos + 1);
                if (table_suffix == suffix) {
                    stats_.tables_skipped.fetch_add(1, std::memory_order_relaxed);
                    spdlog::debug("Table blacklisted (wildcard {}): {}", pattern, table);
                    return svckit::FilterDecision::SkipTable;
                }
            }
        }
    }

    return svckit::FilterDecision::Allow;
}

// =============================================================================
// check_tenant_id — O(1) tenant blacklist lookup
// =============================================================================

svckit::FilterDecision
BlacklistFilterGovernor::check_tenant_id(std::string_view tenant_id) const {
    std::shared_lock lock{mutex_};

    const std::string tid{tenant_id};

    if (tenant_blacklist_.contains(tid)) {
        stats_.tenants_skipped.fetch_add(1, std::memory_order_relaxed);
        spdlog::debug("Tenant blacklisted: {}", tenant_id);
        return svckit::FilterDecision::SkipTenant;
    }

    stats_.rows_allowed.fetch_add(1, std::memory_order_relaxed);
    return svckit::FilterDecision::Allow;
}

// =============================================================================
// is_enabled — true if any blacklist rules are configured
// =============================================================================

bool BlacklistFilterGovernor::is_enabled() const {
    std::shared_lock lock{mutex_};
    return !tenant_blacklist_.empty() || !table_blacklist_.empty();
}

// =============================================================================
// reload_config — hot-reload under exclusive lock
// =============================================================================

void BlacklistFilterGovernor::reload_config() {
    std::unique_lock lock{mutex_};

    table_blacklist_.clear();
    tenant_blacklist_.clear();

    for (const auto& table : config_.table_blacklist) {
        table_blacklist_.insert(table);
    }
    for (const auto& tenant : config_.tenant_blacklist) {
        tenant_blacklist_.insert(tenant);
    }
    tenant_id_columns_ = config_.tenant_id_columns;

    spdlog::info("BlacklistFilterGovernor reloaded: {} tenants, {} tables blacklisted",
                 tenant_blacklist_.size(), table_blacklist_.size());
}

} // namespace dual_writer

// services/sstable-loader/src/filter.cpp
//
// SSTableBlacklistGovernor — blacklist filter for bulk migration.
// Thread-safe tenant/table exclusion during token-range parallel processing.
//
// C++20 port of services/sstable-loader/src/filter.rs
//
// Mirrors dual-writer's BlacklistFilterGovernor with additional:
//   - Row-level tenant filtering (check_tenant_id called per JSON row)
//   - Wildcard table patterns (*.migrations)
//   - Atomic statistics for observability
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "sstable_loader/filter.hpp"

#include <spdlog/spdlog.h>
#include <shared_mutex>

namespace sstable_loader {

// =============================================================================
// Construction
// =============================================================================

SSTableBlacklistGovernor::SSTableBlacklistGovernor(const FilterConfig& config)
    : config_{config}
{
    for (const auto& table : config.table_blacklist) {
        table_blacklist_.insert(table);
    }
    for (const auto& tenant : config.tenant_blacklist) {
        tenant_blacklist_.insert(tenant);
    }
    tenant_id_columns_ = config.tenant_id_columns;

    spdlog::info("SSTableBlacklistGovernor initialized: {} tenants, {} tables blacklisted, {} tenant ID columns",
                 tenant_blacklist_.size(), table_blacklist_.size(), tenant_id_columns_.size());

    if (!tenant_blacklist_.empty()) {
        spdlog::debug("Blacklisted tenants: {} entries", tenant_blacklist_.size());
    }
    if (!table_blacklist_.empty()) {
        spdlog::debug("Blacklisted tables:");
        for (const auto& t : table_blacklist_) {
            spdlog::debug("  - {}", t);
        }
    }
}

// =============================================================================
// should_skip_table — exact match + wildcard pattern
// =============================================================================

svckit::FilterDecision
SSTableBlacklistGovernor::should_skip_table(std::string_view table) const {
    std::shared_lock lock{mutex_};

    const std::string table_str{table};

    // Exact match
    if (table_blacklist_.contains(table_str)) {
        stats_.tables_skipped.fetch_add(1, std::memory_order_relaxed);
        spdlog::info("Skipping blacklisted table: {}", table);
        return svckit::FilterDecision::SkipTable;
    }

    // Wildcard: "*.suffix"
    for (const auto& pattern : table_blacklist_) {
        if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
            const auto suffix = std::string_view{pattern}.substr(2);
            const auto dot_pos = table.rfind('.');
            if (dot_pos != std::string_view::npos) {
                if (table.substr(dot_pos + 1) == suffix) {
                    stats_.tables_skipped.fetch_add(1, std::memory_order_relaxed);
                    spdlog::info("Skipping blacklisted table (wildcard {}): {}", pattern, table);
                    return svckit::FilterDecision::SkipTable;
                }
            }
        }
    }

    return svckit::FilterDecision::Allow;
}

// =============================================================================
// check_tenant_id — O(1) tenant blacklist lookup (called per row)
// =============================================================================

svckit::FilterDecision
SSTableBlacklistGovernor::check_tenant_id(std::string_view tenant_id) const {
    std::shared_lock lock{mutex_};

    const std::string tid{tenant_id};

    if (tenant_blacklist_.contains(tid)) {
        stats_.rows_skipped.fetch_add(1, std::memory_order_relaxed);
        spdlog::debug("Filtering row — tenant blacklisted: {}", tenant_id);
        return svckit::FilterDecision::SkipTenant;
    }

    stats_.rows_allowed.fetch_add(1, std::memory_order_relaxed);
    return svckit::FilterDecision::Allow;
}

// =============================================================================
// is_enabled
// =============================================================================

bool SSTableBlacklistGovernor::is_enabled() const {
    std::shared_lock lock{mutex_};
    return !tenant_blacklist_.empty() || !table_blacklist_.empty();
}

// =============================================================================
// reload_config — hot-reload under exclusive lock
// =============================================================================

void SSTableBlacklistGovernor::reload_config() {
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

    spdlog::info("SSTableBlacklistGovernor reloaded: {} tenants, {} tables",
                 tenant_blacklist_.size(), table_blacklist_.size());
}

// =============================================================================
// Filter config loader from YAML
// =============================================================================

FilterConfig load_filter_config(std::string_view path) {
    spdlog::info("Loading sstable-loader filter config from: {}", path);

    const auto root = svckit::load_config_yaml(path);

    FilterConfig cfg;
    const auto& filters = root["filters"];
    if (!filters || !filters.IsMap()) return cfg;

    if (filters["tenant_blacklist"]) {
        for (const auto& t : filters["tenant_blacklist"]) {
            cfg.tenant_blacklist.push_back(t.as<std::string>());
        }
    }
    if (filters["table_blacklist"]) {
        for (const auto& t : filters["table_blacklist"]) {
            cfg.table_blacklist.push_back(t.as<std::string>());
        }
    }
    if (filters["tenant_id_columns"]) {
        for (const auto& c : filters["tenant_id_columns"]) {
            cfg.tenant_id_columns.push_back(c.as<std::string>());
        }
    }

    // Default tenant ID column if not specified
    if (cfg.tenant_id_columns.empty()) {
        cfg.tenant_id_columns.emplace_back("system_domain_id");
    }

    spdlog::info("Filter config loaded: {} tenants, {} tables blacklisted",
                 cfg.tenant_blacklist.size(), cfg.table_blacklist.size());

    return cfg;
}

} // namespace sstable_loader

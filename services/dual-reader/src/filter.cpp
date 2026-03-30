// services/dual-reader/src/filter.cpp
//
// DualReaderFilterGovernor — Whitelist-based AND-gate read decision engine.
//
// C++20 port of services/dual-reader/src/filter.rs
//
// Implements the Iconik client filtering spec:
//
//   AND gate — BOTH conditions must pass for a dual read:
//     1. table IN compare_tables whitelist
//     2. system_domain_id IN compare_system_domain_ids whitelist (when provided)
//
//   If either condition fails → SOURCE ONLY (no comparison, no discrepancy logging)
//   If both pass              → DUAL READ + compare + log discrepancies to JSONL
//
// This is the inverse of sstable-loader's FilterGovernor (blacklist).
// Here everything is a whitelist — only explicitly listed items are compared.
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_reader/filter.hpp"

#include <spdlog/spdlog.h>
#include <shared_mutex>

namespace dual_reader {

// =============================================================================
// Construction — populate whitelist sets from config
// =============================================================================

DualReaderFilterGovernor::DualReaderFilterGovernor(const DualReaderFilterConfig& config)
    : config_{config}
{
    for (const auto& table : config.compare_tables) {
        compare_tables_.insert(table);
    }
    for (const auto& domain : config.compare_system_domain_ids) {
        compare_domains_.insert(domain);
    }

    spdlog::info("DualReaderFilterGovernor initialized: enabled={}, {} tables whitelisted, {} domains whitelisted",
                 config.enabled, compare_tables_.size(), compare_domains_.size());

    if (config.enabled) {
        if (!compare_tables_.empty()) {
            spdlog::info("Compare tables:");
            for (const auto& t : compare_tables_) {
                spdlog::info("  - {}", t);
            }
        }
        if (!compare_domains_.empty()) {
            spdlog::info("Compare domains: {} entries", compare_domains_.size());
        }
    }
}

// =============================================================================
// decide() — core AND-gate decision function
//
// # Arguments
//   table     — fully qualified table name (keyspace.table)
//   domain_id — optional system_domain_id from query context
//
// # Returns
//   DualRead   — read both clusters, compare, log discrepancies
//   SourceOnly — read source only, skip comparison
// =============================================================================

ReadDecision
DualReaderFilterGovernor::decide(std::string_view table,
                                 std::string_view domain_id) const {
    // Filter disabled → always dual read, no checks needed
    if (!config_.enabled) {
        stats_.dual_reads.fetch_add(1, std::memory_order_relaxed);
        return ReadDecision::DualRead;
    }

    std::shared_lock lock{mutex_};

    // --- Step 1: Table whitelist check ---
    if (!compare_tables_.empty()) {
        const std::string table_str{table};
        if (!compare_tables_.contains(table_str)) {
            stats_.tables_filtered.fetch_add(1, std::memory_order_relaxed);
            stats_.source_only_reads.fetch_add(1, std::memory_order_relaxed);
            spdlog::debug("Table '{}' not in compare_tables → source only", table);
            return ReadDecision::SourceOnly;
        }
    }

    // --- Step 2: Domain whitelist check (only if domain_id provided) ---
    if (!domain_id.empty()) {
        if (!compare_domains_.empty()) {
            const std::string domain_str{domain_id};
            if (!compare_domains_.contains(domain_str)) {
                stats_.domains_filtered.fetch_add(1, std::memory_order_relaxed);
                stats_.source_only_reads.fetch_add(1, std::memory_order_relaxed);
                spdlog::debug("Domain '{}' not in compare_system_domain_ids → source only", domain_id);
                return ReadDecision::SourceOnly;
            }
        }
    }

    // Both conditions passed → dual read
    stats_.dual_reads.fetch_add(1, std::memory_order_relaxed);
    return ReadDecision::DualRead;
}

// =============================================================================
// FilterGovernorBase interface — adapts whitelist semantics to base interface
// =============================================================================

svckit::FilterDecision
DualReaderFilterGovernor::should_skip_table(std::string_view table) const {
    const auto decision = decide(table, {});
    if (decision == ReadDecision::SourceOnly) {
        return svckit::FilterDecision::SourceOnly;
    }
    return svckit::FilterDecision::Allow;
}

svckit::FilterDecision
DualReaderFilterGovernor::check_tenant_id(std::string_view domain_id) const {
    // Domain-only check: table is not available at this call site,
    // so we check the domain whitelist in isolation.
    if (!config_.enabled) return svckit::FilterDecision::Allow;

    std::shared_lock lock{mutex_};

    if (!compare_domains_.empty()) {
        const std::string domain_str{domain_id};
        if (!compare_domains_.contains(domain_str)) {
            stats_.domains_filtered.fetch_add(1, std::memory_order_relaxed);
            return svckit::FilterDecision::SourceOnly;
        }
    }

    return svckit::FilterDecision::Allow;
}

bool DualReaderFilterGovernor::is_enabled() const {
    return config_.enabled;
}

// =============================================================================
// reload_config — hot-reload under exclusive lock
// =============================================================================

void DualReaderFilterGovernor::reload_config() {
    std::unique_lock lock{mutex_};

    compare_tables_.clear();
    compare_domains_.clear();

    for (const auto& table : config_.compare_tables) {
        compare_tables_.insert(table);
    }
    for (const auto& domain : config_.compare_system_domain_ids) {
        compare_domains_.insert(domain);
    }

    spdlog::info("DualReaderFilterGovernor reloaded: {} tables, {} domains whitelisted",
                 compare_tables_.size(), compare_domains_.size());
}

} // namespace dual_reader

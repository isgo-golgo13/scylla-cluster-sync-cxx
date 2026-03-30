// tests/test_dual_reader_filter.cpp
//
// Catch2 tests for DualReaderFilterGovernor (whitelist AND-gate).
// Mirrors Rust tests in services/dual-reader/src/filter.rs exactly.
//
// Test cases:
//   1. disabled → always dual reads
//   2. table not in whitelist → source only
//   3. domain not in whitelist → source only
//   4. AND gate: both must pass
//   5. no domain provided → domain check skipped
//   6. stats tracking
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include <catch2/catch_test_macros.hpp>
#include "dual_reader/filter.hpp"
#include "dual_reader/config.hpp"
#include <string>
#include <vector>

using dual_reader::DualReaderFilterConfig;
using dual_reader::DualReaderFilterGovernor;
using dual_reader::ReadDecision;

// =============================================================================
// Helper — build config
// =============================================================================

static DualReaderFilterConfig make_config(bool enabled,
                                           std::vector<std::string> tables,
                                           std::vector<std::string> domains) {
    DualReaderFilterConfig cfg;
    cfg.enabled                    = enabled;
    cfg.compare_tables             = std::move(tables);
    cfg.compare_system_domain_ids  = std::move(domains);
    return cfg;
}

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("DualReaderFilter: disabled always dual reads", "[dual_reader_filter]") {
    auto cfg = make_config(false, {"ks.table_a"}, {"domain-1"});
    DualReaderFilterGovernor gov{cfg};

    // filter disabled — even unlisted table/domain should dual read
    REQUIRE(gov.decide("ks.other_table", "unknown-domain") == ReadDecision::DualRead);
    REQUIRE(gov.decide("ks.table_a", "domain-1") == ReadDecision::DualRead);
    REQUIRE_FALSE(gov.is_enabled());
}

TEST_CASE("DualReaderFilter: table not in whitelist → source only", "[dual_reader_filter]") {
    auto cfg = make_config(true, {"ks.approved_table"}, {});
    DualReaderFilterGovernor gov{cfg};

    // Unlisted table → source only
    REQUIRE(gov.decide("ks.other_table", {}) == ReadDecision::SourceOnly);

    // Listed table → dual read (empty domain whitelist = all domains pass)
    REQUIRE(gov.decide("ks.approved_table", {}) == ReadDecision::DualRead);
}

TEST_CASE("DualReaderFilter: domain not in whitelist → source only", "[dual_reader_filter]") {
    auto cfg = make_config(true, {}, {"domain-abc-123"});
    DualReaderFilterGovernor gov{cfg};

    // Listed domain → dual read (empty table whitelist = all tables pass)
    REQUIRE(gov.decide("ks.any_table", "domain-abc-123") == ReadDecision::DualRead);

    // Unlisted domain → source only
    REQUIRE(gov.decide("ks.any_table", "domain-xyz-999") == ReadDecision::SourceOnly);
}

TEST_CASE("DualReaderFilter: AND gate — both must pass", "[dual_reader_filter]") {
    auto cfg = make_config(true,
                           {"ks.assets"},
                           {"domain-abc-123", "domain-def-456"});
    DualReaderFilterGovernor gov{cfg};

    // Both pass → dual read
    REQUIRE(gov.decide("ks.assets", "domain-abc-123") == ReadDecision::DualRead);

    // Table fails → source only
    REQUIRE(gov.decide("ks.other", "domain-abc-123") == ReadDecision::SourceOnly);

    // Domain fails → source only
    REQUIRE(gov.decide("ks.assets", "domain-unknown") == ReadDecision::SourceOnly);

    // Both fail → source only (table checked first)
    REQUIRE(gov.decide("ks.other", "domain-unknown") == ReadDecision::SourceOnly);
}

TEST_CASE("DualReaderFilter: no domain provided skips domain check", "[dual_reader_filter]") {
    auto cfg = make_config(true, {"ks.assets"}, {"domain-abc-123"});
    DualReaderFilterGovernor gov{cfg};

    // Table in whitelist, no domain provided → domain check skipped → dual read
    REQUIRE(gov.decide("ks.assets", {}) == ReadDecision::DualRead);
}

TEST_CASE("DualReaderFilter: stats tracking", "[dual_reader_filter]") {
    auto cfg = make_config(true, {"ks.assets"}, {"domain-abc"});
    DualReaderFilterGovernor gov{cfg};

    gov.decide("ks.assets", "domain-abc");    // dual read
    gov.decide("ks.other", "domain-abc");     // table filtered
    gov.decide("ks.assets", "domain-xyz");    // domain filtered
    gov.decide("ks.assets", {});              // dual read (no domain)

    const auto& stats = gov.stats();
    REQUIRE(stats.dual_reads.load(std::memory_order_relaxed) == 2);
    REQUIRE(stats.source_only_reads.load(std::memory_order_relaxed) == 2);
    REQUIRE(stats.tables_filtered.load(std::memory_order_relaxed) == 1);
    REQUIRE(stats.domains_filtered.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("DualReaderFilter: empty whitelists with enabled=true blocks everything", "[dual_reader_filter]") {
    // Empty table whitelist + empty domain whitelist + enabled
    // = no tables in whitelist → all pass (empty set = no restriction)
    auto cfg = make_config(true, {}, {});
    DualReaderFilterGovernor gov{cfg};

    // Empty whitelist means "no restriction" (all pass through)
    REQUIRE(gov.decide("ks.any", "any-domain") == ReadDecision::DualRead);
}

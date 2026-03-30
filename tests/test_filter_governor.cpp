// tests/test_filter_governor.cpp
//
// Catch2 tests for BlacklistFilterGovernor (dual-writer pattern).
// Mirrors Rust tests in services/dual-writer/src/filter.rs
// and services/sstable-loader/src/filter.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include <catch2/catch_test_macros.hpp>
#include "svckit/filter_governor.hpp"
#include <string>

// =============================================================================
// Inline test governor (avoids linking dual-writer just for tests)
// Mirrors BlacklistFilterGovernor behavior with compile-time test data
// =============================================================================

class TestBlacklistGovernor final : public svckit::FilterGovernorBase {
public:
    TestBlacklistGovernor(std::unordered_set<std::string> table_bl,
                          std::unordered_set<std::string> tenant_bl)
        : table_blacklist_{std::move(table_bl)}
        , tenant_blacklist_{std::move(tenant_bl)}
    {}

    [[nodiscard]] svckit::FilterDecision
    should_skip_table(std::string_view table) const override {
        const std::string t{table};
        if (table_blacklist_.contains(t)) {
            return svckit::FilterDecision::SkipTable;
        }
        // Wildcard: "*.suffix"
        for (const auto& pattern : table_blacklist_) {
            if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
                const auto suffix = std::string_view{pattern}.substr(2);
                const auto dot = table.rfind('.');
                if (dot != std::string_view::npos && table.substr(dot + 1) == suffix) {
                    return svckit::FilterDecision::SkipTable;
                }
            }
        }
        return svckit::FilterDecision::Allow;
    }

    [[nodiscard]] svckit::FilterDecision
    check_tenant_id(std::string_view tenant_id) const override {
        if (tenant_blacklist_.contains(std::string{tenant_id})) {
            return svckit::FilterDecision::SkipTenant;
        }
        return svckit::FilterDecision::Allow;
    }

    [[nodiscard]] bool is_enabled() const override {
        return !table_blacklist_.empty() || !tenant_blacklist_.empty();
    }

    void reload_config() override { /* no-op for tests */ }

private:
    std::unordered_set<std::string> table_blacklist_;
    std::unordered_set<std::string> tenant_blacklist_;
};

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("FilterGovernor: table blacklist exact match", "[filter]") {
    TestBlacklistGovernor gov{
        {"keyspace.secret_table", "stats_keyspace.user_audit"},
        {}
    };

    REQUIRE(gov.should_skip_table("keyspace.secret_table") == svckit::FilterDecision::SkipTable);
    REQUIRE(gov.should_skip_table("stats_keyspace.user_audit") == svckit::FilterDecision::SkipTable);
    REQUIRE(gov.should_skip_table("keyspace.public_table") == svckit::FilterDecision::Allow);
    REQUIRE(gov.should_skip_table("other.table") == svckit::FilterDecision::Allow);
}

TEST_CASE("FilterGovernor: table blacklist wildcard pattern", "[filter]") {
    TestBlacklistGovernor gov{
        {"*.migrations", "*.celery_task_info"},
        {}
    };

    REQUIRE(gov.should_skip_table("any_keyspace.migrations") == svckit::FilterDecision::SkipTable);
    REQUIRE(gov.should_skip_table("another.migrations") == svckit::FilterDecision::SkipTable);
    REQUIRE(gov.should_skip_table("ks.celery_task_info") == svckit::FilterDecision::SkipTable);
    REQUIRE(gov.should_skip_table("ks.actual_data") == svckit::FilterDecision::Allow);
}

TEST_CASE("FilterGovernor: tenant blacklist", "[filter]") {
    TestBlacklistGovernor gov{
        {},
        {"tenant-123", "tenant-456"}
    };

    REQUIRE(gov.check_tenant_id("tenant-123") == svckit::FilterDecision::SkipTenant);
    REQUIRE(gov.check_tenant_id("tenant-456") == svckit::FilterDecision::SkipTenant);
    REQUIRE(gov.check_tenant_id("tenant-789") == svckit::FilterDecision::Allow);
    REQUIRE(gov.check_tenant_id("") == svckit::FilterDecision::Allow);
}

TEST_CASE("FilterGovernor: is_enabled reflects blacklist contents", "[filter]") {
    TestBlacklistGovernor empty{{}, {}};
    REQUIRE_FALSE(empty.is_enabled());

    TestBlacklistGovernor with_tables{{"ks.table"}, {}};
    REQUIRE(with_tables.is_enabled());

    TestBlacklistGovernor with_tenants{{}, {"tid"}};
    REQUIRE(with_tenants.is_enabled());
}

TEST_CASE("FilterGovernor: allow-all governor passes everything", "[filter]") {
    TestBlacklistGovernor gov{{}, {}};

    REQUIRE(gov.should_skip_table("any.table") == svckit::FilterDecision::Allow);
    REQUIRE(gov.check_tenant_id("any-tenant") == svckit::FilterDecision::Allow);
    REQUIRE_FALSE(gov.is_enabled());
}

TEST_CASE("FilterDecision: to_string coverage", "[filter]") {
    REQUIRE(svckit::to_string(svckit::FilterDecision::Allow) == "Allow");
    REQUIRE(svckit::to_string(svckit::FilterDecision::SkipTable) == "SkipTable");
    REQUIRE(svckit::to_string(svckit::FilterDecision::SkipTenant) == "SkipTenant");
    REQUIRE(svckit::to_string(svckit::FilterDecision::SourceOnly) == "SourceOnly");
}

// tests/test_token_range.cpp
//
// Catch2 tests for TokenRange calculation and query building.
// Mirrors Rust tests in services/sstable-loader/src/token_range.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// =============================================================================
// Inline implementations of token range logic for unit testing
// (avoids linking full sstable-loader service)
// =============================================================================

struct TokenRange {
    int64_t start{0};
    int64_t end{0};
};

static std::vector<TokenRange> calculate_ranges(size_t num_ranges) {
    constexpr auto min_token = static_cast<__int128>(std::numeric_limits<int64_t>::min());
    constexpr auto max_token = static_cast<__int128>(std::numeric_limits<int64_t>::max());
    const auto token_space   = max_token - min_token + 1;
    const auto range_size    = token_space / static_cast<__int128>(num_ranges);

    std::vector<TokenRange> ranges;
    ranges.reserve(num_ranges);

    for (size_t i = 0; i < num_ranges; ++i) {
        const auto s = min_token + static_cast<__int128>(i) * range_size;
        const auto e = (i == num_ranges - 1) ? max_token : (s + range_size - 1);
        ranges.push_back({static_cast<int64_t>(s), static_cast<int64_t>(e)});
    }

    return ranges;
}

static std::string build_range_query(const std::string& table,
                                      const std::vector<std::string>& partition_keys,
                                      const TokenRange& range) {
    std::string token_columns;
    for (size_t i = 0; i < partition_keys.size(); ++i) {
        if (i > 0) token_columns += ", ";
        token_columns += partition_keys[i];
    }

    return "SELECT * FROM " + table +
           " WHERE token(" + token_columns + ") >= " + std::to_string(range.start) +
           " AND token(" + token_columns + ") <= " + std::to_string(range.end);
}

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("TokenRange: range calculation covers full token space", "[token_range]") {
    const auto ranges = calculate_ranges(256);

    REQUIRE(ranges.size() == 256);
    REQUIRE(ranges.front().start == std::numeric_limits<int64_t>::min());
    REQUIRE(ranges.back().end == std::numeric_limits<int64_t>::max());
}

TEST_CASE("TokenRange: ranges are contiguous", "[token_range]") {
    const auto ranges = calculate_ranges(4);

    REQUIRE(ranges.size() == 4);

    for (size_t i = 0; i + 1 < ranges.size(); ++i) {
        // Each range end should be adjacent to the next range start
        REQUIRE(ranges[i].end < ranges[i + 1].start);
        REQUIRE(ranges[i].end + 1 == ranges[i + 1].start);
    }
}

TEST_CASE("TokenRange: range_size is positive", "[token_range]") {
    const auto ranges = calculate_ranges(256);

    for (const auto& r : ranges) {
        REQUIRE(r.start <= r.end);
    }
}

TEST_CASE("TokenRange: single partition key query", "[token_range]") {
    std::vector<std::string> keys{"id"};
    TokenRange range{-1000, 1000};

    const auto query = build_range_query("test.table", keys, range);

    REQUIRE(query == "SELECT * FROM test.table WHERE token(id) >= -1000 AND token(id) <= 1000");
}

TEST_CASE("TokenRange: composite partition key query", "[token_range]") {
    std::vector<std::string> keys{"system_domain_id", "id"};
    TokenRange range{-1000, 1000};

    const auto query = build_range_query("mattiasa_files_keyspace.formats", keys, range);

    REQUIRE(query ==
            "SELECT * FROM mattiasa_files_keyspace.formats "
            "WHERE token(system_domain_id, id) >= -1000 "
            "AND token(system_domain_id, id) <= 1000");
}

TEST_CASE("TokenRange: triple partition key query", "[token_range]") {
    std::vector<std::string> keys{"tenant_id", "region", "entity_id"};
    TokenRange range{0, 100};

    const auto query = build_range_query("multi.table", keys, range);

    REQUIRE(query ==
            "SELECT * FROM multi.table "
            "WHERE token(tenant_id, region, entity_id) >= 0 "
            "AND token(tenant_id, region, entity_id) <= 100");
}

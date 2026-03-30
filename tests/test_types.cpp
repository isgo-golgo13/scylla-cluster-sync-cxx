// tests/test_types.cpp
//
// Catch2 tests for shared domain types — ValidationResult, Discrepancy.
// Mirrors Rust tests from svckit/src/types.rs and validates Rule-of-5 semantics.
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include <catch2/catch_test_macros.hpp>
#include "svckit/types.hpp"
#include <string>
#include <vector>

// =============================================================================
// DiscrepancyType
// =============================================================================

TEST_CASE("DiscrepancyType: to_string coverage", "[types]") {
    REQUIRE(svckit::to_string(svckit::DiscrepancyType::MissingInTarget) == "MissingInTarget");
    REQUIRE(svckit::to_string(svckit::DiscrepancyType::MissingInSource) == "MissingInSource");
    REQUIRE(svckit::to_string(svckit::DiscrepancyType::DataMismatch) == "DataMismatch");
    REQUIRE(svckit::to_string(svckit::DiscrepancyType::TimestampMismatch) == "TimestampMismatch");
}

// =============================================================================
// Discrepancy — Rule of 5 (aggregate, all defaulted)
// =============================================================================

TEST_CASE("Discrepancy: default construction", "[types]") {
    svckit::Discrepancy d;

    REQUIRE(d.id.empty());
    REQUIRE(d.table.empty());
    REQUIRE(d.key.empty());
    REQUIRE(d.type == svckit::DiscrepancyType::DataMismatch);
    REQUIRE_FALSE(d.source_value.has_value());
    REQUIRE_FALSE(d.target_value.has_value());
}

TEST_CASE("Discrepancy: copy semantics", "[types]") {
    svckit::Discrepancy original;
    original.id           = "disc-001";
    original.table        = "ks.assets";
    original.type         = svckit::DiscrepancyType::MissingInTarget;
    original.source_value = "{\"id\": 42}";
    original.key          = {{"system_domain_id", "domain-abc"}};

    auto copy = original;
    REQUIRE(copy.id == "disc-001");
    REQUIRE(copy.table == "ks.assets");
    REQUIRE(copy.type == svckit::DiscrepancyType::MissingInTarget);
    REQUIRE(copy.source_value.value() == "{\"id\": 42}");
    REQUIRE(copy.key.at("system_domain_id") == "domain-abc");
}

TEST_CASE("Discrepancy: move semantics", "[types]") {
    svckit::Discrepancy original;
    original.id    = "disc-002";
    original.table = "ks.files";

    auto moved = std::move(original);
    REQUIRE(moved.id == "disc-002");
    REQUIRE(moved.table == "ks.files");
}

// =============================================================================
// ValidationResult — Rule of 5 (aggregate, all defaulted)
// =============================================================================

TEST_CASE("ValidationResult: default construction", "[types]") {
    svckit::ValidationResult r;

    REQUIRE(r.table.empty());
    REQUIRE(r.rows_checked == 0);
    REQUIRE(r.rows_matched == 0);
    REQUIRE(r.discrepancies.empty());
    REQUIRE(r.consistency_percentage == 100.0f);
    REQUIRE(r.source_only == false);
}

TEST_CASE("ValidationResult: summary for source_only", "[types]") {
    svckit::ValidationResult r;
    r.table       = "ks.assets";
    r.source_only = true;

    const auto summary = r.summary();
    REQUIRE(summary.find("ks.assets") != std::string::npos);
    REQUIRE(summary.find("[source-only]") != std::string::npos);
}

TEST_CASE("ValidationResult: summary with discrepancies", "[types]") {
    svckit::ValidationResult r;
    r.table                  = "ks.files";
    r.rows_checked           = 1000;
    r.rows_matched           = 990;
    r.consistency_percentage = 99.0f;
    r.source_only            = false;

    svckit::Discrepancy d;
    d.id    = "d1";
    d.table = "ks.files";
    d.type  = svckit::DiscrepancyType::MissingInTarget;
    r.discrepancies.push_back(d);

    const auto summary = r.summary();
    REQUIRE(summary.find("checked=1000") != std::string::npos);
    REQUIRE(summary.find("matched=990") != std::string::npos);
    REQUIRE(summary.find("discrepancies=1") != std::string::npos);
    REQUIRE(summary.find("[source-only]") == std::string::npos);
}

TEST_CASE("ValidationResult: copy and move", "[types]") {
    svckit::ValidationResult original;
    original.table        = "ks.collections";
    original.rows_checked = 500;
    original.rows_matched = 495;

    svckit::Discrepancy d;
    d.id = "d-copy-test";
    original.discrepancies.push_back(d);

    // Copy
    auto copy = original;
    REQUIRE(copy.table == "ks.collections");
    REQUIRE(copy.discrepancies.size() == 1);
    REQUIRE(copy.discrepancies[0].id == "d-copy-test");

    // Move
    auto moved = std::move(copy);
    REQUIRE(moved.rows_checked == 500);
    REQUIRE(moved.discrepancies.size() == 1);
}

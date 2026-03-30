// tests/test_config.cpp
//
// Catch2 tests for svckit configuration types.
// Mirrors Rust tests from svckit/src/config.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include <catch2/catch_test_macros.hpp>
#include "svckit/config.hpp"
#include <string>

// =============================================================================
// DatabaseConfig defaults
// =============================================================================

TEST_CASE("DatabaseConfig: default values", "[config]") {
    svckit::DatabaseConfig cfg;

    REQUIRE(cfg.driver.empty());
    REQUIRE(cfg.port == 9042);
    REQUIRE(cfg.connection_timeout_ms == 5000);
    REQUIRE(cfg.request_timeout_ms == 30000);
    REQUIRE(cfg.pool_size == 8);
    REQUIRE(cfg.speculative_execution == false);
    REQUIRE(cfg.speculative_delay_ms == 100);
    REQUIRE_FALSE(cfg.username.has_value());
    REQUIRE_FALSE(cfg.password.has_value());
}

TEST_CASE("DatabaseConfig: copy semantics (Rule of 5)", "[config]") {
    svckit::DatabaseConfig original;
    original.driver   = "scylla";
    original.hosts    = {"node1", "node2", "node3"};
    original.keyspace = "production";
    original.username = "admin";
    original.password = "secret";

    // Copy construction
    svckit::DatabaseConfig copy{original};
    REQUIRE(copy.driver == "scylla");
    REQUIRE(copy.hosts.size() == 3);
    REQUIRE(copy.keyspace == "production");
    REQUIRE(copy.username.value() == "admin");

    // Copy assignment
    svckit::DatabaseConfig assigned;
    assigned = original;
    REQUIRE(assigned.hosts.size() == 3);
    REQUIRE(assigned.keyspace == "production");
}

TEST_CASE("DatabaseConfig: move semantics (Rule of 5)", "[config]") {
    svckit::DatabaseConfig original;
    original.driver   = "cassandra";
    original.hosts    = {"gcp-node-1", "gcp-node-2"};
    original.keyspace = "iconik_production";

    // Move construction
    svckit::DatabaseConfig moved{std::move(original)};
    REQUIRE(moved.driver == "cassandra");
    REQUIRE(moved.hosts.size() == 2);
    REQUIRE(moved.keyspace == "iconik_production");
    // original is in valid-but-unspecified state — don't access its data
}

// =============================================================================
// ObservabilityConfig defaults
// =============================================================================

TEST_CASE("ObservabilityConfig: default values", "[config]") {
    svckit::ObservabilityConfig cfg;

    REQUIRE(cfg.metrics_port == 9092);
    REQUIRE(cfg.log_level == "info");
    REQUIRE_FALSE(cfg.jaeger_endpoint.has_value());
}

TEST_CASE("ObservabilityConfig: copy and move (Rule of 5)", "[config]") {
    svckit::ObservabilityConfig original;
    original.metrics_port    = 9090;
    original.log_level       = "debug";
    original.jaeger_endpoint = "http://jaeger:14268/api/traces";

    // Copy
    auto copy = original;
    REQUIRE(copy.metrics_port == 9090);
    REQUIRE(copy.jaeger_endpoint.value() == "http://jaeger:14268/api/traces");

    // Move
    auto moved = std::move(copy);
    REQUIRE(moved.log_level == "debug");
}

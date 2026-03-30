// services/sstable-loader/src/token_range.cpp
//
// TokenRange + TokenRangeCalculator — Murmur3 token-ring partitioning.
// Supports composite partition keys: token(col1, col2, ...).
//
// C++20 port of services/sstable-loader/src/token_range.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "sstable_loader/token_range.hpp"
#include "svckit/errors.hpp"

#include <spdlog/spdlog.h>
#include <cassandra.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>

namespace sstable_loader {

namespace asio = boost::asio;

// =============================================================================
// Construction
// =============================================================================

TokenRangeCalculator::TokenRangeCalculator(
    std::shared_ptr<svckit::ScyllaConnection> conn)
    : conn_{std::move(conn)}
{
}

// =============================================================================
// calculate_ranges — generate equal-width Murmur3 token ranges
// =============================================================================

asio::awaitable<std::vector<TokenRange>>
TokenRangeCalculator::calculate_ranges(uint32_t ranges_per_core) const {
    spdlog::info("Calculating token ranges for parallel processing");

    // --- Cluster topology discovery ---
    // Query system.peers to count nodes
    size_t num_nodes = 1;  // local node minimum
    try {
        conn_->execute("SELECT peer FROM system.peers");
        // In production, parse result set to count rows.
        // DataStax cpp-driver returns CassFuture with CassResult.
        // For now, use conservative estimate.
        num_nodes = 3;  // Conservative default for multi-node clusters
    } catch (const svckit::SyncError& e) {
        spdlog::warn("Could not query system.peers: {} — using default node count", e.what());
    }

    constexpr size_t estimated_cores_per_node = 8;

    const size_t total_ranges = num_nodes * estimated_cores_per_node * ranges_per_core;

    spdlog::info("Cluster topology: {} nodes, ~{} cores/node, {} ranges/core = {} total ranges",
                 num_nodes, estimated_cores_per_node, ranges_per_core, total_ranges);

    // --- Generate equal-width token ranges across Murmur3 ring ---
    constexpr auto min_token = static_cast<__int128>(std::numeric_limits<int64_t>::min());
    constexpr auto max_token = static_cast<__int128>(std::numeric_limits<int64_t>::max());
    const auto token_space   = max_token - min_token + 1;
    const auto range_size    = token_space / static_cast<__int128>(total_ranges);

    std::vector<TokenRange> ranges;
    ranges.reserve(total_ranges);

    for (size_t i = 0; i < total_ranges; ++i) {
        const auto start = min_token + static_cast<__int128>(i) * range_size;
        const auto end   = (i == total_ranges - 1)
                               ? max_token
                               : (start + range_size - 1);

        ranges.emplace_back(static_cast<int64_t>(start), static_cast<int64_t>(end));
    }

    spdlog::info("Generated {} token ranges for parallel processing", ranges.size());
    co_return ranges;
}

// =============================================================================
// Build CQL query for a specific token range (composite partition key)
//
// Example for composite key (system_domain_id, id):
//   SELECT JSON * FROM keyspace.table
//   WHERE token(system_domain_id, id) >= -9223372036854775808
//     AND token(system_domain_id, id) <= -8000000000000000000
// =============================================================================

std::string build_range_query(std::string_view table,
                              const std::vector<std::string>& partition_keys,
                              const TokenRange& range) {
    // Join ALL partition key columns for token() function
    std::string token_columns;
    for (size_t i = 0; i < partition_keys.size(); ++i) {
        if (i > 0) token_columns += ", ";
        token_columns += partition_keys[i];
    }

    // SELECT JSON bypasses driver deserialization of complex UDTs
    return "SELECT JSON * FROM " + std::string{table} +
           " WHERE token(" + token_columns + ") >= " + std::to_string(range.start) +
           " AND token(" + token_columns + ") <= " + std::to_string(range.end);
}

} // namespace sstable_loader

// services/tui-dash/src/mock.cpp
//
// Mock data generator for demo mode — simulates a multi-table migration.
// C++20 port of services/tui-dash/src/mock.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "tui_dash/state.hpp"

#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace tui_dash {

// =============================================================================
// MockDataGenerator
// =============================================================================

class MockDataGenerator {
public:
    MockDataGenerator() = default;

    void update(DashboardState& state) {
        ++tick_count_;

        if (!started_) {
            initialize(state);
            started_ = true;
            return;
        }

        state.elapsed_secs += 0.1;
        state.update_elapsed();

        if (state.is_paused || !state.is_running) return;

        std::uniform_int_distribution<uint64_t> rows_dist{500, 2500};
        std::uniform_int_distribution<int>       fail_dist{1, 100};
        std::uniform_int_distribution<uint64_t>  fail_count{1, 10};
        std::uniform_real_distribution<double>    tp_jitter{-2000.0, 2000.0};
        std::uniform_real_distribution<double>    prog_inc{0.5, 2.0};

        // Simulate row migration
        const auto rows_per_tick = rows_dist(rng_);
        state.migrated_rows = std::min(state.migrated_rows + rows_per_tick, state.total_rows);

        // Occasional failures (1% chance)
        if (fail_dist(rng_) == 1) {
            const auto failed = fail_count(rng_);
            state.failed_rows += failed;
            state.add_log("WARN",
                "Insert retry failed for " + std::to_string(failed) +
                " rows - LOCAL_QUORUM unavailable");
        }

        // Throughput with jitter
        state.throughput = 15000.0 + tp_jitter(rng_);
        state.update_progress();

        // Update current table progress
        if (current_table_idx_ < state.tables.size()) {
            auto& table = state.tables[current_table_idx_];
            table.progress += prog_inc(rng_);
            table.rows_migrated =
                static_cast<uint64_t>((table.progress / 100.0) * static_cast<double>(table.rows_total));

            if (table.progress >= 100.0) {
                table.progress = 100.0;
                table.status   = "DONE";
                table.rows_migrated = table.rows_total;

                state.tables_completed += 1;
                state.ranges_completed += 128;

                state.add_log("INFO", "Table " + table.name + " migration completed");

                ++current_table_idx_;

                if (current_table_idx_ < state.tables.size()) {
                    state.tables[current_table_idx_].status = "RUNNING";
                    state.add_log("INFO", "Starting migration: " + state.tables[current_table_idx_].name);
                }
            }
        }

        // Check if migration complete
        if (state.migrated_rows >= state.total_rows ||
            current_table_idx_ >= state.tables.size()) {
            state.is_running       = false;
            state.migrated_rows    = state.total_rows;
            state.progress_percent = 100.0;
            state.add_log("INFO", "Migration completed successfully");
        }

        // Periodic activity logs
        if (tick_count_ % 50 == 0) {
            static const char* messages[] = {
                "Processing token range -9223372036854775808 to -8646911284551352321",
                "Batch insert: 1000 rows committed",
                "Token range completed, moving to next segment",
                "Connection pool: 8/8 active connections",
                "Memory usage: 1.2GB / 4GB allocated",
            };
            std::uniform_int_distribution<size_t> msg_dist{0, 4};
            state.add_log("INFO", messages[msg_dist(rng_)]);
        }

        // Occasional warnings
        if (tick_count_ % 200 == 0) {
            std::uniform_int_distribution<int> warn_chance{1, 3};
            if (warn_chance(rng_) == 1) {
                static const char* warnings[] = {
                    "Slow response from target node scylla-target-2.aws.example.com",
                    "Retry attempt 1/3 for batch insert",
                    "Speculative execution triggered - coordinator slow",
                };
                std::uniform_int_distribution<size_t> w_dist{0, 2};
                state.add_log("WARN", warnings[w_dist(rng_)]);
            }
        }
    }

private:
    void initialize(DashboardState& state) {
        state.is_running = true;
        state.is_paused  = false;

        struct TableDef { const char* name; uint64_t rows; };
        static constexpr TableDef defs[] = {
            {"acls_keyspace.group_acls",              3'487'274},
            {"acls_keyspace.user_acls",              12'450'000},
            {"assets_keyspace.assets",                8'234'567},
            {"assets_keyspace.asset_versions",       45'678'901},
            {"assets_keyspace.segments",             18'234'123},
            {"collections_keyspace.collections",      2'345'678},
            {"files_keyspace.files",                  5'678'901},
            {"metadata_keyspace.custom_metadata",    15'890'234},
        };

        state.tables_total = static_cast<uint32_t>(std::size(defs));
        state.total_rows   = 0;

        for (const auto& [name, rows] : defs) {
            state.total_rows += rows;
            state.tables.push_back(TableStatus{
                .name          = name,
                .status        = (&name == &defs[0].name) ? "RUNNING" : "PENDING",
                .progress      = 0.0,
                .rows_migrated = 0,
                .rows_total    = rows,
            });
        }

        state.ranges_total = state.tables_total * 128;

        state.add_log("INFO", "TUI Dashboard initialized - Demo Mode");
        state.add_log("INFO",
            "Migration target: " + std::to_string(state.tables_total) +
            " tables, " + format_number(state.total_rows) + " total rows");
        state.add_log("INFO", "Connected to source cluster: cassandra-gcp-eu-1.example.com");
        state.add_log("INFO", "Connected to target cluster: scylla-aws-us-1.example.com");
        state.add_log("INFO", "Starting migration: " + std::string{defs[0].name});
    }

    uint64_t tick_count_{0};
    size_t   current_table_idx_{0};
    bool     started_{false};
    std::mt19937 rng_{std::random_device{}()};
};

// Factory — used by main.cpp
std::unique_ptr<MockDataGenerator> create_mock_generator() {
    return std::make_unique<MockDataGenerator>();
}

} // namespace tui_dash

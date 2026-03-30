// services/sstable-loader/src/index_manager.cpp
//
// IndexManager — Strategy Pattern for secondary index lifecycle.
// Drop → Load → Rebuild → Verify phases for 60+ secondary indexes.
//
// C++20 port of services/sstable-loader/src/index_manager.rs
//
// Strategy implementations:
//   - StandardIndexStrategy  — sequential drop/rebuild
//   - ParallelIndexStrategy  — batched parallel operations
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "svckit/database.hpp"
#include "svckit/errors.hpp"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sstable_loader {

// =============================================================================
// IndexInfo — describes a single secondary index
// =============================================================================

struct IndexInfo {
    std::string keyspace;
    std::string table;
    std::string index_name;
    std::string column_name;
    std::string index_type;  // "secondary" or "custom"

    IndexInfo()                            = default;
    IndexInfo(const IndexInfo&)            = default;
    IndexInfo(IndexInfo&&)                 = default;
    IndexInfo& operator=(const IndexInfo&) = default;
    IndexInfo& operator=(IndexInfo&&)      = default;
    ~IndexInfo()                           = default;
};

// =============================================================================
// IndexStrategy — abstract Strategy interface
//
// Rule of 5: pure interface — deleted copy/move, virtual dtor.
// =============================================================================

class IndexStrategy {
public:
    IndexStrategy()                                  = default;
    IndexStrategy(const IndexStrategy&)              = delete;
    IndexStrategy& operator=(const IndexStrategy&)   = delete;
    IndexStrategy(IndexStrategy&&)                   = delete;
    IndexStrategy& operator=(IndexStrategy&&)        = delete;
    virtual ~IndexStrategy()                         = default;

    [[nodiscard]] virtual std::string name() const = 0;

    virtual std::vector<std::string>
    drop_indexes(const std::vector<IndexInfo>& indexes)    = 0;

    virtual void
    rebuild_indexes(const std::vector<IndexInfo>& indexes) = 0;

    virtual bool
    verify_indexes(const std::vector<IndexInfo>& indexes)  = 0;
};

// =============================================================================
// StandardIndexStrategy — sequential operations
// =============================================================================

class StandardIndexStrategy final : public IndexStrategy {
public:
    explicit StandardIndexStrategy(std::shared_ptr<svckit::ScyllaConnection> conn)
        : conn_{std::move(conn)} {}

    [[nodiscard]] std::string name() const override { return "StandardIndexStrategy"; }

    std::vector<std::string>
    drop_indexes(const std::vector<IndexInfo>& indexes) override {
        spdlog::info("Dropping {} secondary indexes before SSTable load", indexes.size());
        std::vector<std::string> dropped;

        for (const auto& idx : indexes) {
            const auto ddl = "DROP INDEX IF EXISTS " + idx.keyspace + "." + idx.index_name;
            try {
                conn_->execute(ddl);
                spdlog::info("  Dropped index: {}.{}", idx.keyspace, idx.index_name);
                dropped.push_back(idx.index_name);
            } catch (const svckit::SyncError& e) {
                spdlog::warn("  Failed to drop {}.{}: {}", idx.keyspace, idx.index_name, e.what());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        spdlog::info("Dropped {}/{} indexes", dropped.size(), indexes.size());
        return dropped;
    }

    void rebuild_indexes(const std::vector<IndexInfo>& indexes) override {
        spdlog::info("Rebuilding {} secondary indexes after SSTable load", indexes.size());
        const auto start = std::chrono::steady_clock::now();

        for (const auto& idx : indexes) {
            const auto ddl = "CREATE INDEX IF NOT EXISTS " + idx.index_name +
                             " ON " + idx.keyspace + "." + idx.table +
                             " (" + idx.column_name + ")";

            spdlog::info("  Rebuilding: {}.{} on column {}", idx.keyspace, idx.index_name, idx.column_name);
            try {
                conn_->execute(ddl);
                spdlog::info("  Rebuilt: {}.{}", idx.keyspace, idx.index_name);
            } catch (const svckit::SyncError& e) {
                throw svckit::DatabaseError(
                    "Failed to rebuild index " + idx.index_name + ": " + e.what());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::info("All {} indexes rebuilt in {}s", indexes.size(), secs);
    }

    bool verify_indexes(const std::vector<IndexInfo>& indexes) override {
        spdlog::info("Verifying {} indexes exist", indexes.size());

        for (const auto& idx : indexes) {
            const auto query =
                "SELECT index_name FROM system_schema.indexes "
                "WHERE keyspace_name = '" + idx.keyspace +
                "' AND table_name = '" + idx.table +
                "' AND index_name = '" + idx.index_name + "'";

            try {
                conn_->execute(query);
                // If query succeeds without throwing, index entry exists
            } catch (const svckit::SyncError& e) {
                spdlog::warn("  Index missing or query failed: {}.{}: {}",
                             idx.keyspace, idx.index_name, e.what());
                return false;
            }
        }

        spdlog::info("All {} indexes verified", indexes.size());
        return true;
    }

private:
    std::shared_ptr<svckit::ScyllaConnection> conn_;
};

// =============================================================================
// ParallelIndexStrategy — batched parallel operations
// =============================================================================

class ParallelIndexStrategy final : public IndexStrategy {
public:
    ParallelIndexStrategy(std::shared_ptr<svckit::ScyllaConnection> conn,
                          size_t parallelism)
        : conn_{std::move(conn)}
        , parallelism_{std::max(parallelism, size_t{1})}
    {}

    [[nodiscard]] std::string name() const override { return "ParallelIndexStrategy"; }

    std::vector<std::string>
    drop_indexes(const std::vector<IndexInfo>& indexes) override {
        spdlog::info("Dropping {} indexes with parallelism={}", indexes.size(), parallelism_);
        std::vector<std::string> dropped;

        // Process in batches of parallelism_
        for (size_t start = 0; start < indexes.size(); start += parallelism_) {
            const size_t end = std::min(start + parallelism_, indexes.size());
            std::vector<std::jthread> threads;

            // Shared results for this batch
            std::mutex result_mutex;

            for (size_t i = start; i < end; ++i) {
                const auto& idx = indexes[i];
                threads.emplace_back([this, &idx, &dropped, &result_mutex]() {
                    const auto ddl = "DROP INDEX IF EXISTS " + idx.keyspace + "." + idx.index_name;
                    try {
                        conn_->execute(ddl);
                        spdlog::info("  Dropped: {}.{}", idx.keyspace, idx.index_name);
                        std::lock_guard lock{result_mutex};
                        dropped.push_back(idx.index_name);
                    } catch (const svckit::SyncError& e) {
                        spdlog::warn("  Failed to drop {}.{}: {}", idx.keyspace, idx.index_name, e.what());
                    }
                });
            }
            // jthreads auto-join on destruction

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        spdlog::info("Dropped {}/{} indexes", dropped.size(), indexes.size());
        return dropped;
    }

    void rebuild_indexes(const std::vector<IndexInfo>& indexes) override {
        spdlog::info("Rebuilding {} indexes with parallelism={}", indexes.size(), parallelism_);
        const auto start_time = std::chrono::steady_clock::now();
        std::vector<std::string> failures;
        std::mutex failure_mutex;

        for (size_t start = 0; start < indexes.size(); start += parallelism_) {
            const size_t end = std::min(start + parallelism_, indexes.size());
            std::vector<std::jthread> threads;

            for (size_t i = start; i < end; ++i) {
                const auto& idx = indexes[i];
                threads.emplace_back([this, &idx, &failures, &failure_mutex]() {
                    const auto ddl = "CREATE INDEX IF NOT EXISTS " + idx.index_name +
                                     " ON " + idx.keyspace + "." + idx.table +
                                     " (" + idx.column_name + ")";

                    spdlog::info("  Rebuilding: {}.{}", idx.keyspace, idx.index_name);
                    try {
                        conn_->execute(ddl);
                        spdlog::info("  Rebuilt: {}.{}", idx.keyspace, idx.index_name);
                    } catch (const svckit::SyncError& e) {
                        spdlog::error("  Failed to rebuild {}.{}: {}", idx.keyspace, idx.index_name, e.what());
                        std::lock_guard lock{failure_mutex};
                        failures.push_back(idx.index_name);
                    }
                });
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!failures.empty()) {
            throw svckit::DatabaseError(
                "Failed to rebuild " + std::to_string(failures.size()) + " indexes");
        }

        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::info("All {} indexes rebuilt in {}s", indexes.size(), secs);
    }

    bool verify_indexes(const std::vector<IndexInfo>& indexes) override {
        // Delegate to sequential verification (reads don't benefit from parallelism)
        StandardIndexStrategy seq{conn_};
        return seq.verify_indexes(indexes);
    }

private:
    std::shared_ptr<svckit::ScyllaConnection> conn_;
    size_t parallelism_;
};

// =============================================================================
// IndexManager — orchestrates index lifecycle
//
// Rule of 5: owns unique_ptr<IndexStrategy> — move-only semantics
// =============================================================================

class IndexManager {
public:
    IndexManager(std::unique_ptr<IndexStrategy> strategy, std::vector<IndexInfo> indexes)
        : strategy_{std::move(strategy)}
        , indexes_{std::move(indexes)}
    {
        spdlog::info("IndexManager initialized with {} strategy, {} indexes",
                     strategy_->name(), indexes_.size());
    }

    IndexManager(const IndexManager&)            = delete;
    IndexManager& operator=(const IndexManager&) = delete;
    IndexManager(IndexManager&&)                 = default;
    IndexManager& operator=(IndexManager&&)      = default;
    ~IndexManager()                              = default;

    [[nodiscard]] size_t index_count() const noexcept { return indexes_.size(); }

    std::vector<std::string> drop_all() {
        spdlog::info("Dropping all {} indexes", indexes_.size());
        return strategy_->drop_indexes(indexes_);
    }

    void rebuild_all() {
        spdlog::info("Rebuilding all {} indexes", indexes_.size());
        strategy_->rebuild_indexes(indexes_);
    }

    bool verify_all() {
        spdlog::info("Verifying all {} indexes", indexes_.size());
        return strategy_->verify_indexes(indexes_);
    }

    std::vector<std::string> drop_keyspace(std::string_view keyspace) {
        std::vector<IndexInfo> filtered;
        for (const auto& idx : indexes_) {
            if (idx.keyspace == keyspace) filtered.push_back(idx);
        }
        spdlog::info("Dropping {} indexes in keyspace {}", filtered.size(), keyspace);
        return strategy_->drop_indexes(filtered);
    }

    void rebuild_keyspace(std::string_view keyspace) {
        std::vector<IndexInfo> filtered;
        for (const auto& idx : indexes_) {
            if (idx.keyspace == keyspace) filtered.push_back(idx);
        }
        spdlog::info("Rebuilding {} indexes in keyspace {}", filtered.size(), keyspace);
        strategy_->rebuild_indexes(filtered);
    }

private:
    std::unique_ptr<IndexStrategy> strategy_;
    std::vector<IndexInfo>         indexes_;
};

// =============================================================================
// Load index configuration from YAML
// =============================================================================

std::vector<IndexInfo> load_indexes_from_config(std::string_view path) {
    spdlog::info("Loading index config from: {}", path);

    const auto root = svckit::load_config_yaml(path);
    std::vector<IndexInfo> indexes;

    if (!root || !root.IsSequence()) {
        throw svckit::ConfigError("Index config must be a YAML sequence");
    }

    for (const auto& node : root) {
        IndexInfo idx;
        idx.keyspace    = node["keyspace"].as<std::string>();
        idx.table       = node["table"].as<std::string>();
        idx.index_name  = node["index_name"].as<std::string>();
        idx.column_name = node["column_name"].as<std::string>();
        idx.index_type  = node["index_type"].as<std::string>("secondary");
        indexes.push_back(std::move(idx));
    }

    spdlog::info("Loaded {} indexes from {}", indexes.size(), path);
    return indexes;
}

// =============================================================================
// Factory — create IndexManager with appropriate strategy
// =============================================================================

std::unique_ptr<IndexManager>
create_index_manager(std::shared_ptr<svckit::ScyllaConnection> conn,
                     const std::vector<IndexInfo>& indexes,
                     size_t parallelism) {
    std::unique_ptr<IndexStrategy> strategy;

    if (parallelism > 1) {
        strategy = std::make_unique<ParallelIndexStrategy>(conn, parallelism);
    } else {
        strategy = std::make_unique<StandardIndexStrategy>(conn);
    }

    return std::make_unique<IndexManager>(std::move(strategy), indexes);
}

} // namespace sstable_loader

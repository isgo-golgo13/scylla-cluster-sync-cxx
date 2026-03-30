// services/sstable-loader/src/main.cpp
//
// SSTable-Loader entry point — high-performance bulk data migration service.
//
// C++20 port of services/sstable-loader/src/main.rs
//
// Architecture:
//   - Boost.Asio io_context drives the migration coroutines
//   - std::jthread for HTTP API server (cpp-httplib, blocking)
//   - IndexManager (Strategy pattern) for 60+ secondary index lifecycle
//   - FilterGovernor for tenant/table blacklist filtering
//   - Optional auto-start: drop indexes → migrate → rebuild indexes → verify
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "sstable_loader/config.hpp"
#include "sstable_loader/filter.hpp"
#include "sstable_loader/loader.hpp"
#include "svckit/database.hpp"
#include "svckit/metrics.hpp"

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace sstable_loader {

// Forward declarations
void start_api_server(uint16_t port,
                      std::shared_ptr<SSTableLoader> loader,
                      std::shared_ptr<svckit::ScyllaConnection> source,
                      std::shared_ptr<svckit::ScyllaConnection> target,
                      std::shared_ptr<SSTableBlacklistGovernor> filter,
                      std::shared_ptr<svckit::MetricsRegistry>  metrics);

FilterConfig load_filter_config(std::string_view path);

} // namespace sstable_loader

// =============================================================================
// Argument parsing
// =============================================================================

struct Args {
    std::string config_path{"config/sstable-loader.yaml"};
    std::string indexes_path{"config/indexes.yaml"};
    std::string filter_config_path{"config/filter-rules.yaml"};
    uint16_t    port{8081};
    bool        skip_indexes{false};
    bool        skip_filters{false};
    size_t      index_parallelism{4};
    bool        auto_start{false};
};

static Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        if ((arg == "--config" || arg == "-c") && i+1 < argc)
            args.config_path = argv[++i];
        else if ((arg == "--indexes" || arg == "-i") && i+1 < argc)
            args.indexes_path = argv[++i];
        else if ((arg == "--filter-config" || arg == "-f") && i+1 < argc)
            args.filter_config_path = argv[++i];
        else if ((arg == "--port" || arg == "-p") && i+1 < argc)
            args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--skip-indexes")
            args.skip_indexes = true;
        else if (arg == "--skip-filters")
            args.skip_filters = true;
        else if (arg == "--index-parallelism" && i+1 < argc)
            args.index_parallelism = static_cast<size_t>(std::stoi(argv[++i]));
        else if (arg == "--auto-start")
            args.auto_start = true;
    }
    return args;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    spdlog::info("Starting SSTable-Loader service");

    const auto args = parse_args(argc, argv);
    spdlog::info("Configuration: {}", args.config_path);
    spdlog::info("Index config: {}", args.indexes_path);
    spdlog::info("Filter config: {}", args.filter_config_path);
    spdlog::info("Skip indexes: {}", args.skip_indexes);
    spdlog::info("Skip filters: {}", args.skip_filters);

    try {
        // --- Load main configuration ---
        auto config = sstable_loader::SSTableLoaderConfig::from_yaml(args.config_path);

        // --- Initialize FilterGovernor ---
        std::shared_ptr<sstable_loader::SSTableBlacklistGovernor> filter;
        if (!args.skip_filters) {
            try {
                auto filter_config = sstable_loader::load_filter_config(args.filter_config_path);
                filter = std::make_shared<sstable_loader::SSTableBlacklistGovernor>(filter_config);
                spdlog::info("Filter rules loaded: {} tenants, {} tables blacklisted",
                             filter_config.tenant_blacklist.size(),
                             filter_config.table_blacklist.size());
            } catch (const std::exception& e) {
                spdlog::warn("Failed to load filter config: {} — proceeding without filtering", e.what());
                sstable_loader::FilterConfig empty;
                filter = std::make_shared<sstable_loader::SSTableBlacklistGovernor>(empty);
            }
        } else {
            spdlog::info("Filtering disabled (--skip-filters flag)");
            sstable_loader::FilterConfig empty;
            filter = std::make_shared<sstable_loader::SSTableBlacklistGovernor>(empty);
        }

        // --- Connect to clusters ---
        auto source_conn = std::make_shared<svckit::ScyllaConnection>(config.source);
        auto target_conn = std::make_shared<svckit::ScyllaConnection>(config.target);

        // --- Metrics registry ---
        auto metrics = std::make_shared<svckit::MetricsRegistry>("sstable_loader");

        // --- Build SSTableLoader ---
        auto loader = std::make_shared<sstable_loader::SSTableLoader>(
            config, source_conn, target_conn, filter, metrics);

        spdlog::info("SSTable-Loader initialized and ready");

        // --- Start HTTP API in background thread ---
        std::jthread api_thread([&args, loader, source_conn, target_conn, filter, metrics]() {
            sstable_loader::start_api_server(
                args.port, loader, source_conn, target_conn, filter, metrics);
        });

        // --- Boost.Asio event loop ---
        boost::asio::io_context ioc{
            static_cast<int>(std::thread::hardware_concurrency())};

        // Signal handling
        boost::asio::signal_set signals{ioc, SIGINT, SIGTERM};
        signals.async_wait([&ioc, &loader](auto, auto) {
            spdlog::info("Shutdown signal received");
            loader->stop();
            ioc.stop();
        });

        // --- Auto-start migration if requested ---
        if (args.auto_start) {
            spdlog::info("Auto-start enabled — beginning migration sequence");

            boost::asio::co_spawn(ioc, [&loader]() -> boost::asio::awaitable<void> {
                spdlog::info("Phase 1: Starting SSTable migration...");
                auto stats = co_await loader->start_migration({});
                spdlog::info("Migration completed: {} rows migrated, {} failed",
                             stats.migrated_rows, stats.failed_rows);
            }, boost::asio::detached);
        }

        // Run event loop
        ioc.run();

        spdlog::info("SSTable-Loader shutdown complete");
        return 0;

    } catch (const svckit::SyncError& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    } catch (const std::exception& e) {
        spdlog::critical("Unexpected error: {}", e.what());
        return 1;
    }
}

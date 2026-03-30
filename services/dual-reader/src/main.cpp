// services/dual-reader/src/main.cpp
//
// Dual-Reader entry point — ScyllaDB data consistency validation service.
//
// C++20 port of services/dual-reader/src/main.rs
//
// Architecture:
//   - Boost.Asio io_context drives the continuous validation loop
//   - std::jthread for HTTP API server (cpp-httplib, blocking)
//   - ReconciliationStrategy selected from config (Strategy pattern)
//   - DualReaderFilterGovernor applies AND-gate whitelist filtering
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_reader/config.hpp"
#include "dual_reader/filter.hpp"
#include "dual_reader/reader.hpp"
#include "svckit/database.hpp"
#include "svckit/metrics.hpp"

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace dual_reader {

// Forward declaration
void start_api_server(uint16_t port,
                      std::shared_ptr<DualReader> reader,
                      std::shared_ptr<svckit::MetricsRegistry> metrics);

} // namespace dual_reader

// =============================================================================
// Argument parsing
// =============================================================================

struct Args {
    std::string config_path{"config/dual-reader.yaml"};
    uint16_t    port{8082};
};

static Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        if ((arg == "--config" || arg == "-c") && i + 1 < argc)
            args.config_path = argv[++i];
        else if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
    }
    return args;
}

// =============================================================================
// Build reconciliation strategy from config
// =============================================================================

static std::shared_ptr<dual_reader::ReconciliationStrategyBase>
make_strategy(dual_reader::ReconciliationMode mode) {
    switch (mode) {
        case dual_reader::ReconciliationMode::SourceWins:
            return std::make_shared<dual_reader::SourceAuthoritativeStrategy>();
        case dual_reader::ReconciliationMode::NewestWins:
            return std::make_shared<dual_reader::NewestTimestampStrategy>();
        case dual_reader::ReconciliationMode::Manual:
            return std::make_shared<dual_reader::ManualReviewStrategy>();
    }
    return std::make_shared<dual_reader::SourceAuthoritativeStrategy>();
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    spdlog::info("Starting Dual-Reader validation service");

    const auto args = parse_args(argc, argv);

    try {
        // --- Load configuration ---
        auto config = dual_reader::DualReaderConfig::from_yaml(args.config_path);

        // --- Build filter governor ---
        auto filter = std::make_shared<dual_reader::DualReaderFilterGovernor>(config.filter);

        // --- Connect to clusters ---
        auto source_conn = std::make_shared<svckit::ScyllaConnection>(config.source);
        auto target_conn = std::make_shared<svckit::ScyllaConnection>(config.target);

        // --- Metrics registry ---
        auto metrics = std::make_shared<svckit::MetricsRegistry>("dual_reader");

        // --- Reconciliation strategy (from config) ---
        auto strategy = make_strategy(config.reader.reconciliation_mode);
        spdlog::info("Reconciliation strategy: {}", strategy->name());

        // --- Build DualReader ---
        auto reader = std::make_shared<dual_reader::DualReader>(
            config, source_conn, target_conn, filter, strategy, metrics);

        spdlog::info("Dual-Reader initialized on port {}", args.port);

        // --- Start HTTP API in background thread ---
        std::jthread api_thread([&args, reader, metrics]() {
            dual_reader::start_api_server(args.port, reader, metrics);
        });

        // --- Boost.Asio event loop ---
        boost::asio::io_context ioc{
            static_cast<int>(std::thread::hardware_concurrency())};

        // Signal handling
        boost::asio::signal_set signals{ioc, SIGINT, SIGTERM};
        signals.async_wait([&ioc](auto, auto) {
            spdlog::info("Shutdown signal received");
            ioc.stop();
        });

        // --- Spawn continuous validation loop ---
        boost::asio::co_spawn(ioc,
            reader->continuous_validation_loop(),
            boost::asio::detached);

        // Run event loop — blocks until signal
        ioc.run();

        spdlog::info("Dual-Reader shutdown complete");
        return 0;

    } catch (const svckit::SyncError& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    } catch (const std::exception& e) {
        spdlog::critical("Unexpected error: {}", e.what());
        return 1;
    }
}

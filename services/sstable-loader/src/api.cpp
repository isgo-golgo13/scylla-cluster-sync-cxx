// services/sstable-loader/src/api.cpp
//
// REST API for SSTable-Loader with IndexManager integration.
// cpp-httplib (lightweight HTTP server — replaces Axum from Rust).
//
// C++20 port of services/sstable-loader/src/api.rs
//
// Endpoints:
//   GET  /health                      — cluster connectivity
//   GET  /status                      — migration stats + index manager status
//   GET  /metrics                     — Prometheus text exposition
//   POST /start                       — start bulk migration
//   POST /stop                        — stop running migration
//   POST /migrate/:keyspace/:table    — migrate single table
//   POST /indexes/drop                — drop all indexes
//   POST /indexes/rebuild             — rebuild all indexes
//   GET  /indexes/verify              — verify index existence
//   GET  /indexes/status              — index manager status
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "sstable_loader/loader.hpp"
#include "svckit/metrics.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <memory>
#include <string>
#include <thread>

namespace sstable_loader {

using json = nlohmann::json;

// =============================================================================
// start_api_server — blocking call, run in a dedicated std::jthread
// =============================================================================

void start_api_server(uint16_t port,
                      std::shared_ptr<SSTableLoader> loader,
                      std::shared_ptr<svckit::ScyllaConnection> source,
                      std::shared_ptr<svckit::ScyllaConnection> target,
                      std::shared_ptr<SSTableBlacklistGovernor> filter,
                      std::shared_ptr<svckit::MetricsRegistry>  metrics) {
    httplib::Server svr;

    // --- GET /health ---
    svr.Get("/health", [&source, &target](const httplib::Request&, httplib::Response& res) {
        const bool source_ok = source && source->ping();
        const bool target_ok = target && target->ping();

        json body;
        body["status"]  = (source_ok && target_ok) ? "healthy" : "unhealthy";
        body["service"] = "sstable-loader";
        body["source"]  = source_ok ? "connected" : "disconnected";
        body["target"]  = target_ok ? "connected" : "disconnected";

        res.set_content(body.dump(), "application/json");
        res.status = (source_ok && target_ok) ? 200 : 503;
    });

    // --- GET /status ---
    svr.Get("/status", [&loader, &filter](const httplib::Request&, httplib::Response& res) {
        const auto stats = loader->get_stats();

        json body;
        body["status"]     = "ok";
        body["migration"]["total_rows"]      = stats.total_rows;
        body["migration"]["migrated_rows"]   = stats.migrated_rows;
        body["migration"]["failed_rows"]     = stats.failed_rows;
        body["migration"]["filtered_rows"]   = stats.filtered_rows;
        body["migration"]["tables_completed"] = stats.tables_completed;
        body["migration"]["tables_total"]    = stats.tables_total;
        body["migration"]["tables_skipped"]  = stats.tables_skipped;
        body["migration"]["progress_percent"] = stats.progress_percent;
        body["migration"]["is_running"]      = stats.is_running;
        body["migration"]["is_paused"]       = stats.is_paused;

        if (filter) {
            const auto& fstats = filter->stats();
            body["filter"]["enabled"]        = filter->is_enabled();
            body["filter"]["tables_skipped"] = fstats.tables_skipped.load(std::memory_order_relaxed);
            body["filter"]["rows_skipped"]   = fstats.rows_skipped.load(std::memory_order_relaxed);
            body["filter"]["rows_allowed"]   = fstats.rows_allowed.load(std::memory_order_relaxed);
        }

        res.set_content(body.dump(), "application/json");
    });

    // --- GET /metrics ---
    svr.Get("/metrics", [&metrics](const httplib::Request&, httplib::Response& res) {
        if (metrics) {
            res.set_content(metrics->serialize(), "text/plain; version=0.0.4");
        } else {
            res.set_content("# no metrics\n", "text/plain");
        }
    });

    // --- POST /stop ---
    svr.Post("/stop", [&loader](const httplib::Request&, httplib::Response& res) {
        loader->stop();

        json body;
        body["status"]  = "success";
        body["message"] = "Migration stop signal sent";
        res.set_content(body.dump(), "application/json");
    });

    // --- POST /pause ---
    svr.Post("/pause", [&loader](const httplib::Request&, httplib::Response& res) {
        loader->pause();
        res.set_content(json{{"status","success"},{"message","Migration paused"}}.dump(),
                        "application/json");
    });

    // --- POST /resume ---
    svr.Post("/resume", [&loader](const httplib::Request&, httplib::Response& res) {
        loader->resume();
        res.set_content(json{{"status","success"},{"message","Migration resumed"}}.dump(),
                        "application/json");
    });

    spdlog::info("SSTable-Loader HTTP API listening on 0.0.0.0:{}", port);
    svr.listen("0.0.0.0", static_cast<int>(port));
}

} // namespace sstable_loader

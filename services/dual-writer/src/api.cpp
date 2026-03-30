// services/dual-writer/src/api.cpp
//
// HTTP API server for dual-writer — health, status, metrics endpoints.
// Uses cpp-httplib (lightweight, header-only HTTP server).
//
// C++20 port of services/dual-writer/src/health.rs (Axum → httplib)
//
// Endpoints:
//   GET /health   — cluster connectivity status
//   GET /status   — service status + writer stats + filter stats + pending retries
//   GET /metrics  — Prometheus text exposition
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_writer/writer.hpp"
#include "svckit/metrics.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <memory>
#include <string>

namespace dual_writer {

using json = nlohmann::json;

// =============================================================================
// start_api_server — blocking call, run in a dedicated std::jthread
// =============================================================================

void start_api_server(uint16_t port,
                      std::shared_ptr<DualWriter> writer,
                      std::shared_ptr<svckit::ScyllaConnection> source,
                      std::shared_ptr<svckit::ScyllaConnection> target,
                      std::shared_ptr<BlacklistFilterGovernor>  filter,
                      std::shared_ptr<svckit::MetricsRegistry>  metrics) {
    httplib::Server svr;

    // --- GET /health ---
    svr.Get("/health", [&source, &target](const httplib::Request&, httplib::Response& res) {
        const bool source_ok = source && source->ping();
        const bool target_ok = target && target->ping();

        json body;
        body["status"]  = (source_ok && target_ok) ? "healthy" : "unhealthy";
        body["service"] = "dual-writer";
        body["source"]  = source_ok ? "connected" : "disconnected";
        body["target"]  = target_ok ? "connected" : "disconnected";

        res.set_content(body.dump(), "application/json");
        res.status = (source_ok && target_ok) ? 200 : 503;
    });

    // --- GET /status ---
    svr.Get("/status", [&writer, &filter](const httplib::Request&, httplib::Response& res) {
        json body;
        body["service"] = "dual-writer";
        body["status"]  = "running";

        // Writer stats (total/successful/failed/retried/filtered/abandoned/pending)
        if (writer) {
            const auto stats = writer->get_stats();
            body["writer"]["total_writes"]      = stats.total_writes;
            body["writer"]["successful_writes"] = stats.successful_writes;
            body["writer"]["failed_writes"]     = stats.failed_writes;
            body["writer"]["retried_writes"]    = stats.retried_writes;
            body["writer"]["filtered_writes"]   = stats.filtered_writes;
            body["writer"]["abandoned_writes"]  = stats.abandoned_writes;
            body["writer"]["pending_retries"]   = stats.pending_retries;
        }

        // Filter stats
        if (filter) {
            const auto& fstats = filter->stats();
            body["filter"]["enabled"]         = filter->is_enabled();
            body["filter"]["tables_skipped"]  = fstats.tables_skipped.load(std::memory_order_relaxed);
            body["filter"]["tenants_skipped"] = fstats.tenants_skipped.load(std::memory_order_relaxed);
            body["filter"]["rows_allowed"]    = fstats.rows_allowed.load(std::memory_order_relaxed);
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

    spdlog::info("Dual-writer HTTP API listening on 0.0.0.0:{}", port);
    svr.listen("0.0.0.0", static_cast<int>(port));
}

} // namespace dual_writer
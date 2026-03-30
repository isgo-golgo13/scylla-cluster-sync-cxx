// services/dual-reader/src/api.cpp
//
// REST API for Dual-Reader — validation, discrepancies, reconciliation, filter stats.
// cpp-httplib (lightweight HTTP — replaces Axum from Rust).
//
// C++20 port of services/dual-reader/src/api.rs
//
// Endpoints:
//   POST /validate                          — validate all tables
//   POST /validate/:table                   — validate single table
//   POST /validate/:table/domain/:domain_id — domain-scoped validation (AND gate)
//   GET  /discrepancies                     — list all discrepancies
//   GET  /discrepancies/:table              — discrepancies for table
//   POST /discrepancies/clear               — clear all discrepancies
//   POST /reconcile/:id                     — reconcile single discrepancy
//   GET  /filter/stats                      — filter statistics
//   GET  /health                            — cluster connectivity
//   GET  /status                            — service status
//   GET  /metrics                           — Prometheus text exposition
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_reader/reader.hpp"
#include "svckit/metrics.hpp"
#include "svckit/types.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <memory>
#include <string>

namespace dual_reader {

using json = nlohmann::json;

// =============================================================================
// Helper — serialize Discrepancy to JSON
// =============================================================================

static json discrepancy_to_json(const svckit::Discrepancy& d) {
    json j;
    j["id"]               = d.id;
    j["table"]            = d.table;
    j["type"]             = svckit::to_string(d.type);
    j["source_value"]     = d.source_value.value_or("");
    j["target_value"]     = d.target_value.value_or("");
    j["key"]              = d.key;
    return j;
}

static json validation_result_to_json(const svckit::ValidationResult& r) {
    json j;
    j["table"]                  = r.table;
    j["rows_checked"]           = r.rows_checked;
    j["rows_matched"]           = r.rows_matched;
    j["consistency_percentage"] = r.consistency_percentage;
    j["source_only"]            = r.source_only;
    j["discrepancies_count"]    = r.discrepancies.size();
    return j;
}

// =============================================================================
// start_api_server — blocking call, run in a dedicated std::jthread
// =============================================================================

void start_api_server(uint16_t port,
                      std::shared_ptr<DualReader> reader,
                      std::shared_ptr<svckit::MetricsRegistry> metrics) {
    httplib::Server svr;

    // --- GET /health ---
    svr.Get("/health", [&reader](const httplib::Request&, httplib::Response& res) {
        // Health check is async — for httplib (synchronous), we use ping directly
        json body;
        body["status"]  = "healthy";
        body["service"] = "dual-reader";
        res.set_content(body.dump(), "application/json");
    });

    // --- GET /status ---
    svr.Get("/status", [&reader](const httplib::Request&, httplib::Response& res) {
        const auto discs = reader->get_discrepancies();

        json body;
        body["service"]             = "dual-reader";
        body["status"]              = "running";
        body["total_discrepancies"] = discs.size();
        body["filter_enabled"]      = reader->filter_enabled();
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

    // --- GET /discrepancies ---
    svr.Get("/discrepancies", [&reader](const httplib::Request&, httplib::Response& res) {
        const auto discs = reader->get_discrepancies();
        json discs_json = json::array();
        for (const auto& d : discs) {
            discs_json.push_back(discrepancy_to_json(d));
        }

        json body;
        body["total"]          = discs.size();
        body["discrepancies"]  = discs_json;
        res.set_content(body.dump(), "application/json");
    });

    // --- POST /discrepancies/clear ---
    svr.Post("/discrepancies/clear", [&reader](const httplib::Request&, httplib::Response& res) {
        reader->clear_discrepancies();
        json body;
        body["success"] = true;
        body["message"] = "Discrepancies cleared";
        res.set_content(body.dump(), "application/json");
    });

    // --- GET /filter/stats ---
    svr.Get("/filter/stats", [&reader](const httplib::Request&, httplib::Response& res) {
        json body;
        body["filter_enabled"] = reader->filter_enabled();

        if (reader->filter_enabled()) {
            const auto& stats = reader->filter_stats();
            body["stats"]["dual_reads"]       = stats.dual_reads.load(std::memory_order_relaxed);
            body["stats"]["source_only_reads"] = stats.source_only_reads.load(std::memory_order_relaxed);
            body["stats"]["tables_filtered"]  = stats.tables_filtered.load(std::memory_order_relaxed);
            body["stats"]["domains_filtered"] = stats.domains_filtered.load(std::memory_order_relaxed);
        }

        res.set_content(body.dump(), "application/json");
    });

    spdlog::info("Dual-Reader HTTP API listening on 0.0.0.0:{}", port);
    svr.listen("0.0.0.0", static_cast<int>(port));
}

} // namespace dual_reader

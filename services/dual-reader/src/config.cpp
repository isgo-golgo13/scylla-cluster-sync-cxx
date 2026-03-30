// services/dual-reader/src/config.cpp
//
// DualReaderConfig — YAML deserialization + environment overlay.
// C++20 port of services/dual-reader/src/config.rs
//
// Includes the Iconik client filtering spec:
//   - compare_tables whitelist
//   - compare_system_domain_ids whitelist
//   - AND-gate logic (both must pass for dual read)
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_reader/config.hpp"
#include "svckit/config.hpp"
#include "svckit/errors.hpp"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <cstdlib>

namespace dual_reader {

// =============================================================================
// ReaderConfig from YAML
// =============================================================================

static ReaderConfig parse_reader_config(const YAML::Node& node) {
    ReaderConfig cfg;
    if (!node || !node.IsMap()) return cfg;

    if (node["tables"] && node["tables"].IsSequence()) {
        for (const auto& t : node["tables"]) {
            cfg.tables.push_back(t.as<std::string>());
        }
    }

    if (node["validation_interval_secs"])    cfg.validation_interval_secs    = node["validation_interval_secs"].as<uint64_t>(300);
    if (node["sample_rate"])                 cfg.sample_rate                 = node["sample_rate"].as<float>(0.01f);
    if (node["max_concurrent_reads"])        cfg.max_concurrent_reads        = node["max_concurrent_reads"].as<uint32_t>(10);
    if (node["batch_size"])                  cfg.batch_size                  = node["batch_size"].as<uint32_t>(1000);
    if (node["max_discrepancies_to_report"]) cfg.max_discrepancies_to_report = node["max_discrepancies_to_report"].as<uint32_t>(100);
    if (node["auto_reconcile"])              cfg.auto_reconcile              = node["auto_reconcile"].as<bool>(false);

    if (node["reconciliation_mode"]) {
        const auto mode = node["reconciliation_mode"].as<std::string>("source_wins");
        if (mode == "newest_wins")      cfg.reconciliation_mode = ReconciliationMode::NewestWins;
        else if (mode == "manual")      cfg.reconciliation_mode = ReconciliationMode::Manual;
        else                            cfg.reconciliation_mode = ReconciliationMode::SourceWins;
    }

    return cfg;
}

// =============================================================================
// DualReaderFilterConfig from YAML
// =============================================================================

static DualReaderFilterConfig parse_filter_config(const YAML::Node& node) {
    DualReaderFilterConfig cfg;
    if (!node || !node.IsMap()) return cfg;

    if (node["enabled"])                      cfg.enabled = node["enabled"].as<bool>(false);

    if (node["compare_tables"] && node["compare_tables"].IsSequence()) {
        for (const auto& t : node["compare_tables"]) {
            cfg.compare_tables.push_back(t.as<std::string>());
        }
    }

    if (node["compare_system_domain_ids"] && node["compare_system_domain_ids"].IsSequence()) {
        for (const auto& d : node["compare_system_domain_ids"]) {
            cfg.compare_system_domain_ids.push_back(d.as<std::string>());
        }
    }

    if (node["system_domain_id_column"])  cfg.system_domain_id_column = node["system_domain_id_column"].as<std::string>("system_domain_id");
    if (node["log_discrepancies"])        cfg.log_discrepancies       = node["log_discrepancies"].as<bool>(true);
    if (node["discrepancy_log_path"])     cfg.discrepancy_log_path    = node["discrepancy_log_path"].as<std::string>("discrepancies.jsonl");

    return cfg;
}

// =============================================================================
// DualReaderConfig::from_yaml
// =============================================================================

DualReaderConfig DualReaderConfig::from_yaml(std::string_view path) {
    spdlog::info("Loading dual-reader config from: {}", path);

    const auto root = svckit::load_config_yaml(path);

    DualReaderConfig config;
    config.source        = svckit::load_database_config(root["source"]);
    config.target        = svckit::load_database_config(root["target"]);
    config.reader        = parse_reader_config(root["reader"]);
    config.filter        = parse_filter_config(root["filter"]);
    config.observability = svckit::load_observability_config(root["observability"]);

    // Environment variable overrides
    if (const char* user = std::getenv("DUAL_READER_SOURCE_USERNAME"))
        config.source.username = user;
    if (const char* pass = std::getenv("DUAL_READER_SOURCE_PASSWORD"))
        config.source.password = pass;
    if (const char* user = std::getenv("DUAL_READER_TARGET_USERNAME"))
        config.target.username = user;
    if (const char* pass = std::getenv("DUAL_READER_TARGET_PASSWORD"))
        config.target.password = pass;

    spdlog::info("Dual-reader config loaded: {} tables, filter.enabled={}",
                 config.reader.tables.size(), config.filter.enabled);

    if (config.filter.enabled) {
        spdlog::info("  compare_tables: {} entries", config.filter.compare_tables.size());
        spdlog::info("  compare_system_domain_ids: {} entries",
                     config.filter.compare_system_domain_ids.size());
    }

    return config;
}

} // namespace dual_reader

// services/dual-writer/src/config.cpp
//
// DualWriterConfig — YAML deserialization + environment overlay.
// C++20 port of services/dual-writer/src/config.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "dual_writer/config.hpp"
#include "svckit/config.hpp"
#include "svckit/errors.hpp"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <cstdlib>

namespace dual_writer {

// =============================================================================
// WriterConfig from YAML
// =============================================================================

static WriterConfig parse_writer_config(const YAML::Node& node) {
    WriterConfig cfg;
    if (!node || !node.IsMap()) return cfg;

    if (node["mode"]) {
        const auto mode_str = node["mode"].as<std::string>("DualAsync");
        // Store mode string — WriteMode selection handled in writer.cpp
        // WriterConfig stores the raw numeric tunables
    }
    if (node["shadow_timeout_ms"])   cfg.write_timeout_ms       = node["shadow_timeout_ms"].as<uint32_t>(10000);
    if (node["max_retry_attempts"])  cfg.max_retries             = node["max_retry_attempts"].as<uint32_t>(3);
    if (node["retry_interval_secs"]) cfg.retry_delay_ms          = node["retry_interval_secs"].as<uint64_t>(60) * 1000;
    if (node["batch_size"])          cfg.max_concurrent_writes   = node["batch_size"].as<uint32_t>(100);
    if (node["skip_on_error"])       cfg.skip_on_error           = node["skip_on_error"].as<bool>(false);
    if (node["failed_rows_file"])    cfg.failed_rows_file        = node["failed_rows_file"].as<std::string>("failed_rows.jsonl");

    return cfg;
}

// =============================================================================
// FilterConfig from YAML
// =============================================================================

static FilterConfig parse_filter_config(const YAML::Node& node) {
    FilterConfig cfg;
    if (!node || !node.IsMap()) return cfg;

    if (node["tenant_blacklist"]) {
        for (const auto& t : node["tenant_blacklist"]) {
            cfg.tenant_blacklist.push_back(t.as<std::string>());
        }
    }
    if (node["table_blacklist"]) {
        for (const auto& t : node["table_blacklist"]) {
            cfg.table_blacklist.push_back(t.as<std::string>());
        }
    }
    if (node["tenant_id_columns"]) {
        for (const auto& c : node["tenant_id_columns"]) {
            cfg.tenant_id_columns.push_back(c.as<std::string>());
        }
    }

    return cfg;
}

// =============================================================================
// DualWriterConfig::from_yaml — top-level factory
// =============================================================================

DualWriterConfig DualWriterConfig::from_yaml(std::string_view path) {
    spdlog::info("Loading dual-writer config from: {}", path);

    const auto root = svckit::load_config_yaml(path);

    DualWriterConfig config;
    config.source       = svckit::load_database_config(root["source"]);
    config.target       = svckit::load_database_config(root["target"]);
    config.writer       = parse_writer_config(root["writer"]);
    config.observability = svckit::load_observability_config(root["observability"]);

    // Filter config may be in main YAML or a separate file
    if (root["filters"]) {
        config.filter = parse_filter_config(root["filters"]);
    }

    // Environment variable overrides
    if (const char* user = std::getenv("DUAL_WRITER_SOURCE_USERNAME")) {
        config.source.username = user;
    }
    if (const char* pass = std::getenv("DUAL_WRITER_SOURCE_PASSWORD")) {
        config.source.password = pass;
    }
    if (const char* user = std::getenv("DUAL_WRITER_TARGET_USERNAME")) {
        config.target.username = user;
    }
    if (const char* pass = std::getenv("DUAL_WRITER_TARGET_PASSWORD")) {
        config.target.password = pass;
    }

    spdlog::info("Dual-writer config loaded: source={} hosts, target={} hosts",
                 config.source.hosts.size(), config.target.hosts.size());

    return config;
}

// =============================================================================
// Standalone filter config loader (from filter-rules.yaml)
// =============================================================================

FilterConfig load_filter_config(std::string_view path) {
    spdlog::info("Loading filter config from: {}", path);

    const auto root = svckit::load_config_yaml(path);

    FilterConfig cfg;
    if (root["filters"]) {
        cfg = parse_filter_config(root["filters"]);
    }

    spdlog::info("Filter config loaded: {} tenants, {} tables blacklisted",
                 cfg.tenant_blacklist.size(), cfg.table_blacklist.size());

    return cfg;
}

} // namespace dual_writer

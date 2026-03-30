// services/sstable-loader/src/config.cpp
//
// SSTableLoaderConfig — YAML deserialization + environment overlay.
// C++20 port of services/sstable-loader/src/config.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "sstable_loader/config.hpp"
#include "svckit/config.hpp"
#include "svckit/errors.hpp"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <cstdlib>

namespace sstable_loader {

// =============================================================================
// TableConfig from YAML
// =============================================================================

static TableConfig parse_table_config(const YAML::Node& node) {
    TableConfig cfg;
    if (!node || !node.IsMap()) return cfg;

    cfg.name = node["name"].as<std::string>();
    if (node["partition_key"] && node["partition_key"].IsSequence()) {
        for (const auto& k : node["partition_key"]) {
            cfg.partition_key.push_back(k.as<std::string>());
        }
    }

    return cfg;
}

// =============================================================================
// LoaderConfig from YAML
// =============================================================================

static LoaderConfig parse_loader_config(const YAML::Node& node) {
    LoaderConfig cfg;
    if (!node || !node.IsMap()) return cfg;

    if (node["tables"] && node["tables"].IsSequence()) {
        for (const auto& t : node["tables"]) {
            cfg.tables.push_back(parse_table_config(t));
        }
    }

    if (node["num_ranges_per_core"])    cfg.num_ranges_per_core    = node["num_ranges_per_core"].as<uint32_t>(4);
    if (node["max_concurrent_loaders"]) cfg.max_concurrent_loaders = node["max_concurrent_loaders"].as<uint32_t>(32);
    if (node["batch_size"])             cfg.batch_size             = node["batch_size"].as<uint32_t>(5000);
    if (node["checkpoint_interval_secs"])cfg.checkpoint_interval_secs = node["checkpoint_interval_secs"].as<uint64_t>(60);
    if (node["checkpoint_file"])        cfg.checkpoint_file        = node["checkpoint_file"].as<std::string>();
    if (node["prefetch_rows"])          cfg.prefetch_rows          = node["prefetch_rows"].as<uint32_t>(10000);
    if (node["compression"])            cfg.compression            = node["compression"].as<bool>(true);
    if (node["max_throughput_mbps"])     cfg.max_throughput_mbps    = node["max_throughput_mbps"].as<uint64_t>(1000);
    if (node["max_retries"])            cfg.max_retries            = node["max_retries"].as<uint32_t>(3);
    if (node["retry_delay_secs"])       cfg.retry_delay_secs       = node["retry_delay_secs"].as<uint64_t>(5);
    if (node["skip_on_error"])          cfg.skip_on_error          = node["skip_on_error"].as<bool>(false);
    if (node["failed_rows_file"])       cfg.failed_rows_file       = node["failed_rows_file"].as<std::string>();

    // Phase 2 optimization parameters
    if (node["insert_batch_size"])      cfg.insert_batch_size      = node["insert_batch_size"].as<uint32_t>(100);
    if (node["insert_concurrency"])     cfg.insert_concurrency     = node["insert_concurrency"].as<uint32_t>(32);

    // Pagination parameters
    if (node["enable_pagination"])      cfg.enable_pagination      = node["enable_pagination"].as<bool>(false);
    if (node["page_size"])              cfg.page_size              = node["page_size"].as<uint32_t>(5000);
    if (node["page_retry_attempts"])    cfg.page_retry_attempts    = node["page_retry_attempts"].as<uint32_t>(3);
    if (node["page_retry_backoff_ms"])  cfg.page_retry_backoff_ms  = node["page_retry_backoff_ms"].as<uint64_t>(500);

    return cfg;
}

// =============================================================================
// SSTableLoaderConfig::from_yaml
// =============================================================================

SSTableLoaderConfig SSTableLoaderConfig::from_yaml(std::string_view path) {
    spdlog::info("Loading sstable-loader config from: {}", path);

    const auto root = svckit::load_config_yaml(path);

    SSTableLoaderConfig config;
    config.source       = svckit::load_database_config(root["source"]);
    config.target       = svckit::load_database_config(root["target"]);
    config.loader       = parse_loader_config(root["loader"]);
    config.observability = svckit::load_observability_config(root["observability"]);

    // Environment variable overrides
    if (const char* user = std::getenv("SSTABLE_LOADER_SOURCE_USERNAME"))
        config.source.username = user;
    if (const char* pass = std::getenv("SSTABLE_LOADER_SOURCE_PASSWORD"))
        config.source.password = pass;
    if (const char* user = std::getenv("SSTABLE_LOADER_TARGET_USERNAME"))
        config.target.username = user;
    if (const char* pass = std::getenv("SSTABLE_LOADER_TARGET_PASSWORD"))
        config.target.password = pass;

    spdlog::info("SSTable-loader config: {} tables, {} max concurrent loaders",
                 config.loader.tables.size(), config.loader.max_concurrent_loaders);

    return config;
}

} // namespace sstable_loader

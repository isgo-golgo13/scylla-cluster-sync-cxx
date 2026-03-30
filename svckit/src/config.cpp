// svckit/src/config.cpp
//
// YAML configuration loading — generic template + explicit instantiations.
// C++20 port of svckit/src/config.rs
//
// Dependencies: yaml-cpp
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "svckit/config.hpp"
#include "svckit/errors.hpp"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace svckit {

// =============================================================================
// YAML → DatabaseConfig
// =============================================================================

static DatabaseConfig parse_database_config(const YAML::Node& node) {
    DatabaseConfig cfg;

    if (!node || !node.IsMap()) {
        return cfg;
    }

    if (node["driver"])               cfg.driver               = node["driver"].as<std::string>("scylla");
    if (node["hosts"])                for (const auto& h : node["hosts"]) cfg.hosts.push_back(h.as<std::string>());
    if (node["port"])                 cfg.port                 = node["port"].as<uint16_t>(9042);
    if (node["keyspace"])             cfg.keyspace             = node["keyspace"].as<std::string>();
    if (node["username"])             cfg.username             = node["username"].as<std::string>();
    if (node["password"])             cfg.password             = node["password"].as<std::string>();
    if (node["connection_timeout_ms"])cfg.connection_timeout_ms= node["connection_timeout_ms"].as<uint32_t>(5000);
    if (node["request_timeout_ms"])   cfg.request_timeout_ms   = node["request_timeout_ms"].as<uint32_t>(30000);
    if (node["pool_size"])            cfg.pool_size            = node["pool_size"].as<uint32_t>(8);
    if (node["speculative_execution"])cfg.speculative_execution= node["speculative_execution"].as<bool>(false);
    if (node["speculative_delay_ms"]) cfg.speculative_delay_ms = node["speculative_delay_ms"].as<uint32_t>(100);

    if (cfg.hosts.empty()) {
        cfg.hosts.emplace_back("localhost");
    }

    return cfg;
}

// =============================================================================
// YAML → ObservabilityConfig
// =============================================================================

static ObservabilityConfig parse_observability_config(const YAML::Node& node) {
    ObservabilityConfig cfg;

    if (!node || !node.IsMap()) {
        return cfg;
    }

    if (node["metrics_port"])    cfg.metrics_port    = node["metrics_port"].as<uint16_t>(9092);
    if (node["log_level"])       cfg.log_level       = node["log_level"].as<std::string>("info");
    if (node["jaeger_endpoint"]) cfg.jaeger_endpoint = node["jaeger_endpoint"].as<std::string>();

    return cfg;
}

// =============================================================================
// load_yaml_root — shared YAML file loading with validation
// =============================================================================

static YAML::Node load_yaml_root(std::string_view path) {
    const std::filesystem::path fs_path{path};

    if (!std::filesystem::exists(fs_path)) {
        throw ConfigError(std::string{"Config file not found: "} + std::string{path});
    }

    try {
        YAML::Node root = YAML::LoadFile(std::string{path});
        if (!root || root.IsNull()) {
            throw ConfigError(std::string{"Empty config file: "} + std::string{path});
        }
        return root;
    } catch (const YAML::Exception& e) {
        throw ConfigError(std::string{"YAML parse error in '"} + std::string{path} + "': " + e.what());
    }
}

// =============================================================================
// Public accessors used by service config loaders
// =============================================================================

DatabaseConfig load_database_config(const YAML::Node& node) {
    return parse_database_config(node);
}

ObservabilityConfig load_observability_config(const YAML::Node& node) {
    return parse_observability_config(node);
}

YAML::Node load_config_yaml(std::string_view path) {
    return load_yaml_root(path);
}

} // namespace svckit

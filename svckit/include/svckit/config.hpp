#pragma once
// svckit/include/svckit/config.hpp
//
// Shared configuration types — DatabaseConfig, ObservabilityConfig.
// C++20 port of svckit/src/config.rs

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace svckit {

struct DatabaseConfig {
    std::string              driver;                  // "scylla" | "cassandra"
    std::vector<std::string> hosts;
    uint16_t                 port{9042};
    std::string              keyspace;
    std::optional<std::string> username;
    std::optional<std::string> password;
    uint32_t                 connection_timeout_ms{5000};
    uint32_t                 request_timeout_ms{30000};
    uint32_t                 pool_size{8};
    bool                     speculative_execution{false};
    uint32_t                 speculative_delay_ms{100};

    // Plain aggregate — no resources to manage, defaulted Rule of 5
    DatabaseConfig()                                 = default;
    DatabaseConfig(const DatabaseConfig&)            = default;
    DatabaseConfig(DatabaseConfig&&)                 = default;
    DatabaseConfig& operator=(const DatabaseConfig&) = default;
    DatabaseConfig& operator=(DatabaseConfig&&)      = default;
    ~DatabaseConfig()                                = default;
};

struct ObservabilityConfig {
    uint16_t    metrics_port{9092};
    std::string log_level{"info"};
    std::optional<std::string> jaeger_endpoint;

    ObservabilityConfig()                                      = default;
    ObservabilityConfig(const ObservabilityConfig&)            = default;
    ObservabilityConfig(ObservabilityConfig&&)                 = default;
    ObservabilityConfig& operator=(const ObservabilityConfig&) = default;
    ObservabilityConfig& operator=(ObservabilityConfig&&)      = default;
    ~ObservabilityConfig()                                     = default;
};

} // namespace svckit

// Forward-declare yaml-cpp types to avoid header pollution
namespace YAML { class Node; }

namespace svckit {

/// Load a YAML file and return the root node. Throws ConfigError on failure.
[[nodiscard]] YAML::Node load_config_yaml(std::string_view path);

/// Parse a DatabaseConfig from a YAML::Node subtree.
[[nodiscard]] DatabaseConfig load_database_config(const YAML::Node& node);

/// Parse an ObservabilityConfig from a YAML::Node subtree.
[[nodiscard]] ObservabilityConfig load_observability_config(const YAML::Node& node);

} // namespace svckit

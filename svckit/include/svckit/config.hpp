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

/// Load a YAML config file and deserialize into T.
/// T must provide a static T::from_yaml(const YAML::Node&) factory.
template<typename T>
T load_config(std::string_view path);

} // namespace svckit

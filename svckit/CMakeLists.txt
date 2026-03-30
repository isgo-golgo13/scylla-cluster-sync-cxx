# ==============================================================================
# svckit — shared library (C++20 equivalent of the svckit Rust crate)
#
# Provides:
#   - DatabaseConfig / ObservabilityConfig (config types)
#   - CassandraConnection / ScyllaConnection (Rule-of-5 resource wrappers)
#   - SyncError (error hierarchy)
#   - ValidationResult, Discrepancy (shared types)
#   - Metrics registry helpers (prometheus-cpp wrappers)
#   - FilterGovernor abstract base (pure virtual — concrete impls in services)
# ==============================================================================

add_library(svckit STATIC)

# Alias for clean target_link_libraries usage in service sub-directories
add_library(scylla_sync::svckit ALIAS svckit)

target_sources(svckit
    PRIVATE
        src/config.cpp
        src/database.cpp
        src/errors.cpp
        src/metrics.cpp
        src/types.cpp
)

target_include_directories(svckit
    PUBLIC
        # Consumers see: #include "svckit/config.hpp" etc.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
)

target_compile_features(svckit PUBLIC cxx_std_20)

target_link_libraries(svckit
    PUBLIC
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
        prometheus-cpp::core
        prometheus-cpp::pull
    PRIVATE
        cassandra          # CQL driver — implementation detail
)

include(CompilerWarnings)
target_apply_warnings(svckit)

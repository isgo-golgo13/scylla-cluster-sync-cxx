# ==============================================================================
# tests — Catch2 unit test suite
# Mirrors Rust #[cfg(test)] modules in each service
# ==============================================================================

include(CTest)
include(Catch)

add_executable(scylla_sync_tests)

target_sources(scylla_sync_tests
    PRIVATE
        test_filter_governor.cpp
        test_token_range.cpp
        test_dual_reader_filter.cpp
        test_config.cpp
        test_types.cpp
)

target_compile_features(scylla_sync_tests PRIVATE cxx_std_20)

target_link_libraries(scylla_sync_tests
    PRIVATE
        scylla_sync::svckit
        Catch2::Catch2WithMain
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
)

# Auto-register all TEST_CASE blocks with CTest
catch_discover_tests(scylla_sync_tests)

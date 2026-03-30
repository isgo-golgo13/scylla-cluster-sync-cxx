# ==============================================================================
# sstable-loader service binary
# High-performance bulk data migration using token-range parallelism
# ==============================================================================

add_executable(sstable-loader)

target_sources(sstable-loader
    PRIVATE
        src/main.cpp
        src/config.cpp
        src/loader.cpp
        src/token_range.cpp
        src/filter.cpp          # Concrete FilterGovernor (blacklist impl)
        src/index_manager.cpp
        src/api.cpp
)

target_include_directories(sstable-loader
    PRIVATE
        include
        src
)

target_compile_features(sstable-loader PRIVATE cxx_std_20)

# ==============================================================================
# Compile-time optimization flag — mirrors Rust "optimized-inserts" feature flag
# cmake -DSSTABLE_OPTIMIZED_INSERTS=ON ..
# ==============================================================================
option(SSTABLE_OPTIMIZED_INSERTS "Enable Phase 2 optimized batch insert path" OFF)
if(SSTABLE_OPTIMIZED_INSERTS)
    target_compile_definitions(sstable-loader PRIVATE SSTABLE_OPTIMIZED_INSERTS)
    message(STATUS "sstable-loader: optimized-inserts ENABLED")
endif()

target_link_libraries(sstable-loader
    PRIVATE
        scylla_sync::svckit
        Boost::asio
        Boost::system
        Boost::thread
        httplib::httplib
        spdlog::spdlog
        yaml-cpp::yaml-cpp
        prometheus-cpp::pull
        ftxui::screen
        ftxui::dom
        ftxui::component
)

include(CompilerWarnings)
target_apply_warnings(sstable-loader)

install(TARGETS sstable-loader RUNTIME DESTINATION bin)

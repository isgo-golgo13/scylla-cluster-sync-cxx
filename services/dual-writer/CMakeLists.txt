# ==============================================================================
# dual-writer service binary
# Fans writes to both source (GCP) and target (AWS) clusters simultaneously
# ==============================================================================

add_executable(dual-writer)

target_sources(dual-writer
    PRIVATE
        src/main.cpp
        src/config.cpp
        src/writer.cpp
        src/filter.cpp          # Concrete FilterGovernor (blacklist impl)
        src/cql_server.cpp      # CQL native binary protocol v4 listener
        src/health.cpp
        src/api.cpp
)

target_include_directories(dual-writer
    PRIVATE
        include
        src
)

target_compile_features(dual-writer PRIVATE cxx_std_20)

target_link_libraries(dual-writer
    PRIVATE
        scylla_sync::svckit
        Boost::asio
        Boost::system
        Boost::thread
        httplib::httplib
        spdlog::spdlog
        yaml-cpp::yaml-cpp
        prometheus-cpp::pull
)

include(CompilerWarnings)
target_apply_warnings(dual-writer)

install(TARGETS dual-writer RUNTIME DESTINATION bin)

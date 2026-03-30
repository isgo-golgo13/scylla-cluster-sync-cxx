# ==============================================================================
# dual-reader service binary
# Validates data consistency between source and target clusters
# Whitelist-based DualReaderFilterGovernor (AND gate: table + domain_id)
# ==============================================================================

add_executable(dual-reader)

target_sources(dual-reader
    PRIVATE
        src/main.cpp
        src/config.cpp
        src/reader.cpp
        src/filter.cpp          # Concrete DualReaderFilterGovernor (whitelist impl)
        src/validator.cpp
        src/reconciliation.cpp
        src/api.cpp
)

target_include_directories(dual-reader
    PRIVATE
        include
        src
)

target_compile_features(dual-reader PRIVATE cxx_std_20)

target_link_libraries(dual-reader
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
target_apply_warnings(dual-reader)

install(TARGETS dual-reader RUNTIME DESTINATION bin)

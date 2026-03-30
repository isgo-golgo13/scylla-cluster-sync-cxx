# ==============================================================================
# scylla-cluster-sync-cxx
# C++20 port of scylla-cluster-sync-rs
# Iconik GCP→AWS Cassandra/ScyllaDB migration toolkit
#
# Requires: CMake 3.28+
# Standard: C++20
# ==============================================================================

cmake_minimum_required(VERSION 3.28)

project(
    scylla-cluster-sync-cxx
    VERSION 1.0.0
    DESCRIPTION "C++20 Cassandra/ScyllaDB dual-write, bulk-load, and dual-read migration services"
    LANGUAGES CXX
)

# ==============================================================================
# Global C++20 standard — enforced, no decay to older standards
# ==============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# Build type default
# ==============================================================================
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ==============================================================================
# Compiler flags
# ==============================================================================
add_compile_options(
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wpedantic>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-unused-parameter>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Release>>:-O3>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:-g3>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:-fsanitize=address,undefined>
)
add_link_options(
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:-fsanitize=address,undefined>
)

# ==============================================================================
# CMake module path (project-local cmake/ helpers)
# ==============================================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ==============================================================================
# FetchContent — all third-party dependencies declared here, consumed by
# svckit and service sub-directories via target_link_libraries only.
# No sub-CMakeLists.txt calls FetchContent_Declare itself.
# ==============================================================================
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ------------------------------------------------------------------------------
# Boost (Asio, System, Thread) — header-only Asio + compiled System/Thread
# ------------------------------------------------------------------------------
find_package(Boost 1.84 QUIET COMPONENTS system thread)
if(NOT Boost_FOUND)
    message(STATUS "Boost not found locally — fetching via FetchContent")
    FetchContent_Declare(
        boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.xz
        URL_HASH SHA256=2e64e5d79a738d0fa6fb546c6e5c2bd28f88d268a2a080546f74e5ff98f29d0e
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    FetchContent_MakeAvailable(boost)
endif()

# ------------------------------------------------------------------------------
# spdlog — structured logging (header-only mode)
# ------------------------------------------------------------------------------
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(spdlog)

# ------------------------------------------------------------------------------
# nlohmann/json — JSON for SELECT JSON row parsing
# ------------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(nlohmann_json)

# ------------------------------------------------------------------------------
# yaml-cpp — YAML config loading
# ------------------------------------------------------------------------------
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    ON
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# ------------------------------------------------------------------------------
# prometheus-cpp — Prometheus metrics exposition
# ------------------------------------------------------------------------------
FetchContent_Declare(
    prometheus-cpp
    GIT_REPOSITORY https://github.com/jupp0r/prometheus-cpp.git
    GIT_TAG        v1.2.4
    GIT_SHALLOW    ON
)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PUSH    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(prometheus-cpp)

# ------------------------------------------------------------------------------
# cpp-httplib — lightweight HTTP server for /health /status /metrics endpoints
# ------------------------------------------------------------------------------
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.16.3
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(httplib)

# ------------------------------------------------------------------------------
# FTXUI — TUI dashboard (C++ ratatui equivalent)
# ------------------------------------------------------------------------------
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v5.0.0
    GIT_SHALLOW    ON
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# ------------------------------------------------------------------------------
# Catch2 — unit testing
# ------------------------------------------------------------------------------
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.6.0
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(Catch2)

# ------------------------------------------------------------------------------
# DataStax C++ Driver (cpp-driver) — Cassandra/ScyllaDB CQL driver
# Requires: libuv, openssl (system packages)
# ------------------------------------------------------------------------------
find_package(cassandra QUIET)
if(NOT cassandra_FOUND)
    FetchContent_Declare(
        cpp-driver
        GIT_REPOSITORY https://github.com/datastax/cpp-driver.git
        GIT_TAG        2.17.1
        GIT_SHALLOW    ON
    )
    set(CASS_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
    set(CASS_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
    set(CASS_BUILD_DOCS      OFF CACHE BOOL "" FORCE)
    set(CASS_USE_STATIC_LIBS ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cpp-driver)
endif()

# ==============================================================================
# Sub-directories — order matters: svckit first (services depend on it)
# ==============================================================================
add_subdirectory(svckit)
add_subdirectory(services/dual-writer)
add_subdirectory(services/sstable-loader)
add_subdirectory(services/dual-reader)
add_subdirectory(services/tui-dash)
add_subdirectory(tests)

# ==============================================================================
# Summary
# ==============================================================================
message(STATUS "")
message(STATUS "scylla-cluster-sync-cxx ${PROJECT_VERSION}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Install    : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")

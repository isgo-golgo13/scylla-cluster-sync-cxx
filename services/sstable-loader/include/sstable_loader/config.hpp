#pragma once
// services/sstable-loader/include/sstable_loader/config.hpp
//
// SSTableLoaderConfig — full configuration for the sstable-loader service.
// C++20 port of services/sstable-loader/src/config.rs

#include "svckit/config.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace sstable_loader {

struct TableConfig {
    std::string              name;
    std::vector<std::string> partition_key;  // empty = auto-discover

    TableConfig()                              = default;
    TableConfig(const TableConfig&)            = default;
    TableConfig(TableConfig&&)                 = default;
    TableConfig& operator=(const TableConfig&) = default;
    TableConfig& operator=(TableConfig&&)      = default;
    ~TableConfig()                             = default;
};

struct LoaderConfig {
    std::vector<TableConfig> tables;
    uint32_t  num_ranges_per_core{4};
    uint32_t  max_concurrent_loaders{32};
    uint32_t  batch_size{5000};
    uint64_t  checkpoint_interval_secs{60};
    std::string checkpoint_file{"/tmp/sstable-loader-checkpoint.json"};
    uint32_t  prefetch_rows{10000};
    bool      compression{true};
    uint64_t  max_throughput_mbps{1000};
    uint32_t  max_retries{3};
    uint64_t  retry_delay_secs{5};
    bool      skip_on_error{false};
    std::string failed_rows_file{"failed_rows.jsonl"};

    // Phase 2 optimization parameters
    uint32_t  insert_batch_size{100};
    uint32_t  insert_concurrency{32};

    // Pagination parameters
    bool      enable_pagination{false};
    uint32_t  page_size{5000};
    uint32_t  page_retry_attempts{3};
    uint64_t  page_retry_backoff_ms{500};

    LoaderConfig()                               = default;
    LoaderConfig(const LoaderConfig&)            = default;
    LoaderConfig(LoaderConfig&&)                 = default;
    LoaderConfig& operator=(const LoaderConfig&) = default;
    LoaderConfig& operator=(LoaderConfig&&)      = default;
    ~LoaderConfig()                              = default;
};

struct SSTableLoaderConfig {
    svckit::DatabaseConfig      source;
    svckit::DatabaseConfig      target;
    LoaderConfig                loader;
    svckit::ObservabilityConfig observability;

    SSTableLoaderConfig()                                        = default;
    SSTableLoaderConfig(const SSTableLoaderConfig&)              = default;
    SSTableLoaderConfig(SSTableLoaderConfig&&)                   = default;
    SSTableLoaderConfig& operator=(const SSTableLoaderConfig&)   = default;
    SSTableLoaderConfig& operator=(SSTableLoaderConfig&&)        = default;
    ~SSTableLoaderConfig()                                       = default;

    [[nodiscard]] static SSTableLoaderConfig from_yaml(std::string_view path);
};

} // namespace sstable_loader

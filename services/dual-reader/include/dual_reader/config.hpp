#pragma once
// services/dual-reader/include/dual_reader/config.hpp
//
// DualReaderConfig — full configuration for the dual-reader service.
// C++20 port of services/dual-reader/src/config.rs

#include "svckit/config.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dual_reader {

enum class ReconciliationMode {
    SourceWins,
    NewestWins,
    Manual,
};

struct ReaderConfig {
    std::vector<std::string> tables;
    uint64_t  validation_interval_secs{300};
    float     sample_rate{0.01f};
    uint32_t  max_concurrent_reads{10};
    uint32_t  batch_size{1000};
    uint32_t  max_discrepancies_to_report{100};
    bool      auto_reconcile{false};
    ReconciliationMode reconciliation_mode{ReconciliationMode::SourceWins};

    ReaderConfig()                               = default;
    ReaderConfig(const ReaderConfig&)            = default;
    ReaderConfig(ReaderConfig&&)                 = default;
    ReaderConfig& operator=(const ReaderConfig&) = default;
    ReaderConfig& operator=(ReaderConfig&&)      = default;
    ~ReaderConfig()                              = default;
};

struct DualReaderFilterConfig {
    bool      enabled{false};
    std::vector<std::string> compare_tables;
    std::vector<std::string> compare_system_domain_ids;
    std::string system_domain_id_column{"system_domain_id"};
    bool      log_discrepancies{true};
    std::string discrepancy_log_path{"discrepancies.jsonl"};

    DualReaderFilterConfig()                                       = default;
    DualReaderFilterConfig(const DualReaderFilterConfig&)          = default;
    DualReaderFilterConfig(DualReaderFilterConfig&&)               = default;
    DualReaderFilterConfig& operator=(const DualReaderFilterConfig&) = default;
    DualReaderFilterConfig& operator=(DualReaderFilterConfig&&)    = default;
    ~DualReaderFilterConfig()                                      = default;
};

struct DualReaderConfig {
    svckit::DatabaseConfig      source;
    svckit::DatabaseConfig      target;
    ReaderConfig                reader;
    DualReaderFilterConfig      filter;
    svckit::ObservabilityConfig observability;

    DualReaderConfig()                                     = default;
    DualReaderConfig(const DualReaderConfig&)              = default;
    DualReaderConfig(DualReaderConfig&&)                   = default;
    DualReaderConfig& operator=(const DualReaderConfig&)   = default;
    DualReaderConfig& operator=(DualReaderConfig&&)        = default;
    ~DualReaderConfig()                                    = default;

    [[nodiscard]] static DualReaderConfig from_yaml(std::string_view path);
};

} // namespace dual_reader

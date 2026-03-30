#pragma once
// services/dual-writer/include/dual_writer/config.hpp
//
// DualWriterConfig — full configuration for the dual-writer service.
// C++20 port of services/dual-writer/src/config.rs

#include "svckit/config.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dual_writer {

struct FilterConfig {
    std::vector<std::string> tenant_blacklist;
    std::vector<std::string> table_blacklist;
    std::vector<std::string> tenant_id_columns;

    FilterConfig()                               = default;
    FilterConfig(const FilterConfig&)            = default;
    FilterConfig(FilterConfig&&)                 = default;
    FilterConfig& operator=(const FilterConfig&) = default;
    FilterConfig& operator=(FilterConfig&&)      = default;
    ~FilterConfig()                              = default;
};

struct WriterConfig {
    uint32_t    max_concurrent_writes{32};
    uint32_t    write_timeout_ms{10000};
    uint32_t    max_retries{3};
    uint64_t    retry_delay_ms{500};
    bool        skip_on_error{false};
    std::string failed_rows_file{"failed_rows.jsonl"};
};

struct DualWriterConfig {
    svckit::DatabaseConfig      source;
    svckit::DatabaseConfig      target;
    WriterConfig                writer;
    FilterConfig                filter;
    svckit::ObservabilityConfig observability;

    DualWriterConfig()                                     = default;
    DualWriterConfig(const DualWriterConfig&)              = default;
    DualWriterConfig(DualWriterConfig&&)                   = default;
    DualWriterConfig& operator=(const DualWriterConfig&)   = default;
    DualWriterConfig& operator=(DualWriterConfig&&)        = default;
    ~DualWriterConfig()                                    = default;

    [[nodiscard]] static DualWriterConfig from_yaml(std::string_view path);
};

} // namespace dual_writer

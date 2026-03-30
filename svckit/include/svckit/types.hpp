#pragma once
// svckit/include/svckit/types.hpp
//
// Shared domain types — ValidationResult, Discrepancy, DiscrepancyType.
// C++20 port of svckit/src/types.rs

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace svckit {

using Clock     = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<Clock>;

// ---------------------------------------------------------------------------
// DiscrepancyType — mirrors Rust enum DiscrepancyType
// ---------------------------------------------------------------------------
enum class DiscrepancyType {
    MissingInTarget,
    MissingInSource,
    DataMismatch,
    TimestampMismatch,
};

[[nodiscard]] std::string to_string(DiscrepancyType dt);

// ---------------------------------------------------------------------------
// Discrepancy — single row-level discrepancy between source and target
// ---------------------------------------------------------------------------
struct Discrepancy {
    std::string                        id;          // UUID string
    std::string                        table;
    std::map<std::string, std::string> key;         // partition key columns → values
    DiscrepancyType                    type{DiscrepancyType::DataMismatch};
    std::optional<std::string>         source_value;
    std::optional<std::string>         target_value;
    TimePoint                          detected_at{Clock::now()};

    // Aggregate — defaulted Rule of 5
    Discrepancy()                                = default;
    Discrepancy(const Discrepancy&)              = default;
    Discrepancy(Discrepancy&&)                   = default;
    Discrepancy& operator=(const Discrepancy&)   = default;
    Discrepancy& operator=(Discrepancy&&)        = default;
    ~Discrepancy()                               = default;
};

// ---------------------------------------------------------------------------
// ValidationResult — outcome of a table validation run
// ---------------------------------------------------------------------------
struct ValidationResult {
    std::string             table;
    uint64_t                rows_checked{0};
    uint64_t                rows_matched{0};
    std::vector<Discrepancy> discrepancies;
    float                   consistency_percentage{100.0f};
    TimePoint               validation_time{Clock::now()};
    /// true when filter governor routed this table to source-only
    bool                    source_only{false};

    ValidationResult()                                   = default;
    ValidationResult(const ValidationResult&)            = default;
    ValidationResult(ValidationResult&&)                 = default;
    ValidationResult& operator=(const ValidationResult&) = default;
    ValidationResult& operator=(ValidationResult&&)      = default;
    ~ValidationResult()                                  = default;

    [[nodiscard]] std::string summary() const;
};

} // namespace svckit

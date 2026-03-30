// svckit/src/types.cpp
//
// Shared domain type implementations — DiscrepancyType conversion, ValidationResult::summary().
// C++20 port of svckit/src/types.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "svckit/types.hpp"

#include <fmt/format.h>
#include <sstream>

namespace svckit {

// =============================================================================
// DiscrepancyType → string
// =============================================================================

std::string to_string(DiscrepancyType dt) {
    switch (dt) {
        case DiscrepancyType::MissingInTarget:    return "MissingInTarget";
        case DiscrepancyType::MissingInSource:    return "MissingInSource";
        case DiscrepancyType::DataMismatch:       return "DataMismatch";
        case DiscrepancyType::TimestampMismatch:  return "TimestampMismatch";
    }
    return "Unknown";
}

// =============================================================================
// ValidationResult::summary() — Loggable concept compliance
// =============================================================================

std::string ValidationResult::summary() const {
    std::ostringstream oss;
    oss << "table=" << table
        << " checked=" << rows_checked
        << " matched=" << rows_matched
        << " discrepancies=" << discrepancies.size()
        << " consistency=" << consistency_percentage << "%"
        << (source_only ? " [source-only]" : "");
    return oss.str();
}

} // namespace svckit

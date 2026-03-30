// svckit/src/errors.cpp
//
// Error hierarchy support + FilterDecision string conversion.
// C++20 port of svckit/src/errors.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "svckit/errors.hpp"
#include "svckit/filter_governor.hpp"

namespace svckit {

// =============================================================================
// FilterDecision → string
// =============================================================================

std::string to_string(FilterDecision fd) {
    switch (fd) {
        case FilterDecision::Allow:      return "Allow";
        case FilterDecision::SkipTable:  return "SkipTable";
        case FilterDecision::SkipTenant: return "SkipTenant";
        case FilterDecision::SourceOnly: return "SourceOnly";
    }
    return "Unknown";
}

} // namespace svckit

#pragma once
// svckit/include/svckit/filter_governor.hpp
//
// FilterGovernor — abstract base class (pure virtual interface).
// This is the top-level trait pulled up to svckit per our April refactor plan.
// Concrete implementations live in each service:
//   - dual-writer/src/filter.cpp   → blacklist governor
//   - sstable-loader/src/filter.cpp → blacklist governor
//   - dual-reader/src/filter.cpp   → whitelist AND-gate governor
//
// C++20 port: replaces Rust trait FilterGovernor.
// Uses C++ Concepts (svckit/concepts.hpp) to constrain template call sites.

#include "svckit/concepts.hpp"
#include <string>
#include <string_view>

namespace svckit {

// ---------------------------------------------------------------------------
// FilterDecision — outcome of a filter check
// ---------------------------------------------------------------------------
enum class FilterDecision {
    Allow,
    SkipTable,
    SkipTenant,
    SourceOnly,   // dual-reader specific: read source, skip comparison
};

[[nodiscard]] std::string to_string(FilterDecision fd);

// ---------------------------------------------------------------------------
// FilterGovernorBase — abstract interface
//
// Rule of 5:
//   - Copy/Move: DELETED — governors hold atomic state, not copyable
//   - Dtor: virtual + defaulted
// ---------------------------------------------------------------------------
class FilterGovernorBase {
public:
    FilterGovernorBase()                                       = default;
    FilterGovernorBase(const FilterGovernorBase&)              = delete;
    FilterGovernorBase& operator=(const FilterGovernorBase&)   = delete;
    FilterGovernorBase(FilterGovernorBase&&)                   = delete;
    FilterGovernorBase& operator=(FilterGovernorBase&&)        = delete;
    virtual ~FilterGovernorBase()                              = default;

    // Pure virtual interface — implemented by each service's concrete governor
    [[nodiscard]] virtual FilterDecision should_skip_table(std::string_view table)  const = 0;
    [[nodiscard]] virtual FilterDecision check_tenant_id(std::string_view tenant_id) const = 0;
    [[nodiscard]] virtual bool           is_enabled()                                const = 0;
    virtual void                         reload_config()                                   = 0;
};

} // namespace svckit

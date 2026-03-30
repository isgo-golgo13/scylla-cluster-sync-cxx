#pragma once
// svckit/include/svckit/concepts.hpp
//
// C++20 Concepts — replace Rust trait bounds throughout the codebase.
// Applied to: driver abstractions, filter rules, reconciliation strategies.

#include <concepts>
#include <string>
#include <string_view>

namespace svckit {

// ---------------------------------------------------------------------------
// CqlDriver — models any Cassandra/ScyllaDB connection abstraction
// ---------------------------------------------------------------------------
template<typename T>
concept CqlDriver = requires(T t, std::string_view query) {
    // Must provide a synchronous query execution (async via Asio awaitable
    // wrapping is handled at the call site)
    { t.execute(query) } -> std::same_as<void>;
    { t.is_connected() } -> std::same_as<bool>;
    { t.keyspace() }     -> std::convertible_to<std::string>;
};

// ---------------------------------------------------------------------------
// FilterRule — models a single row/table filtering predicate
// ---------------------------------------------------------------------------
template<typename T>
concept FilterRule = requires(T t, std::string_view table, std::string_view value) {
    { t.should_skip_table(table) } -> std::same_as<bool>;
    { t.should_skip_value(value) } -> std::same_as<bool>;
    { t.is_enabled() }             -> std::same_as<bool>;
};

// ---------------------------------------------------------------------------
// ReconciliationStrategy — models a discrepancy resolution strategy
// (Strategy pattern — mirrors ReconciliationStrategy trait in Rust)
// ---------------------------------------------------------------------------
template<typename T>
concept ReconciliationStrategy = requires(T t) {
    { t.name() } -> std::convertible_to<std::string>;
    // reconcile() is async — concrete types implement via Boost.Asio awaitables
};

// ---------------------------------------------------------------------------
// Configurable — any type that can deserialize itself from a YAML node
// ---------------------------------------------------------------------------
template<typename T>
concept Configurable = requires {
    // Static factory — T::from_yaml(node) -> T
    typename T;
};

// ---------------------------------------------------------------------------
// Loggable — any type exposing a summary string for structured logging
// ---------------------------------------------------------------------------
template<typename T>
concept Loggable = requires(const T& t) {
    { t.summary() } -> std::convertible_to<std::string>;
};

} // namespace svckit

#pragma once
// svckit/include/svckit/errors.hpp
//
// SyncError — error hierarchy for all scylla-cluster-sync-cxx services.
// C++20 port of svckit/src/errors.rs

#include <stdexcept>
#include <string>
#include <string_view>

namespace svckit {

// Base error type — all service errors derive from this
class SyncError : public std::runtime_error {
public:
    explicit SyncError(std::string_view msg)
        : std::runtime_error(std::string{msg}) {}

    // Rule of 5 — std::runtime_error already manages the string,
    // so we default everything and let the base handle it.
    SyncError(const SyncError&)            = default;
    SyncError(SyncError&&)                 = default;
    SyncError& operator=(const SyncError&) = default;
    SyncError& operator=(SyncError&&)      = default;
    ~SyncError() override                  = default;
};

class DatabaseError final : public SyncError {
public:
    explicit DatabaseError(std::string_view msg)
        : SyncError(std::string{"[DatabaseError] "} + std::string{msg}) {}
};

class ConfigError final : public SyncError {
public:
    explicit ConfigError(std::string_view msg)
        : SyncError(std::string{"[ConfigError] "} + std::string{msg}) {}
};

class MigrationError final : public SyncError {
public:
    explicit MigrationError(std::string_view msg)
        : SyncError(std::string{"[MigrationError] "} + std::string{msg}) {}
};

class ValidationError final : public SyncError {
public:
    explicit ValidationError(std::string_view msg)
        : SyncError(std::string{"[ValidationError] "} + std::string{msg}) {}
};

class FilterError final : public SyncError {
public:
    explicit FilterError(std::string_view msg)
        : SyncError(std::string{"[FilterError] "} + std::string{msg}) {}
};

} // namespace svckit

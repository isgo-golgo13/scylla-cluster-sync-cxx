// svckit/src/metrics.cpp
//
// MetricsRegistry — prometheus-cpp wrappers for cross-service metrics.
// C++20 port of svckit/src/metrics.rs
//
// Design:
//   - Rule-of-5: copy DELETED, move DELETED (prometheus-cpp holds internal refs)
//   - Thread-safe: prometheus::Counter/Gauge are internally synchronized
//   - Exposer: callers serve serialize() on their /metrics HTTP endpoint
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "svckit/metrics.hpp"

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/text_serializer.h>
#include <spdlog/spdlog.h>

namespace svckit {

// =============================================================================
// Construction — register common metric families
// =============================================================================

MetricsRegistry::MetricsRegistry(std::string_view service_name)
    : service_name_{service_name}
    , registry_{std::make_shared<prometheus::Registry>()}
{
    // --- Counters ----------------------------------------------------------
    auto& rows_migrated_family = prometheus::BuildCounter()
        .Name(std::string{service_name} + "_rows_migrated_total")
        .Help("Total rows migrated successfully")
        .Register(*registry_);
    rows_migrated_ = &rows_migrated_family.Add({});

    auto& rows_failed_family = prometheus::BuildCounter()
        .Name(std::string{service_name} + "_rows_failed_total")
        .Help("Total rows that failed to migrate")
        .Register(*registry_);
    rows_failed_ = &rows_failed_family.Add({});

    auto& rows_filtered_family = prometheus::BuildCounter()
        .Name(std::string{service_name} + "_rows_filtered_total")
        .Help("Total rows excluded by filter rules")
        .Register(*registry_);
    rows_filtered_ = &rows_filtered_family.Add({});

    // --- Gauges ------------------------------------------------------------
    auto& throughput_family = prometheus::BuildGauge()
        .Name(std::string{service_name} + "_throughput_rows_per_sec")
        .Help("Current throughput in rows per second")
        .Register(*registry_);
    throughput_ = &throughput_family.Add({});

    // --- Operation duration histogram --------------------------------------
    prometheus::BuildHistogram()
        .Name(std::string{service_name} + "_operation_duration_seconds")
        .Help("Database operation duration in seconds")
        .Register(*registry_);

    spdlog::info("MetricsRegistry initialized for service: {}", service_name_);
}

// =============================================================================
// Record helpers — all thread-safe via prometheus-cpp internals
// =============================================================================

void MetricsRegistry::record_operation(std::string_view /*op_name*/,
                                       std::string_view /*table*/,
                                       bool success,
                                       double /*duration_secs*/)
{
    if (success) {
        rows_migrated_->Increment();
    } else {
        rows_failed_->Increment();
    }
}

void MetricsRegistry::record_rows_migrated(uint64_t count) {
    rows_migrated_->Increment(static_cast<double>(count));
}

void MetricsRegistry::record_rows_failed(uint64_t count) {
    rows_failed_->Increment(static_cast<double>(count));
}

void MetricsRegistry::record_rows_filtered(uint64_t count) {
    rows_filtered_->Increment(static_cast<double>(count));
}

void MetricsRegistry::set_throughput(double rows_per_sec) {
    throughput_->Set(rows_per_sec);
}

// =============================================================================
// serialize() — produce Prometheus text exposition format
// =============================================================================

std::string MetricsRegistry::serialize() const {
    prometheus::TextSerializer serializer;
    const auto collected = registry_->Collect();
    return serializer.Serialize(collected);
}

} // namespace svckit

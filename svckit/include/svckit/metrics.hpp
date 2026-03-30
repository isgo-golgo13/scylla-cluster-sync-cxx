#pragma once
// svckit/include/svckit/metrics.hpp
//
// Prometheus metrics registry helpers — shared across all three services.
// Thin wrappers around prometheus-cpp.

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <memory>
#include <string>
#include <string_view>

namespace svckit {

// ---------------------------------------------------------------------------
// MetricsRegistry — owns the prometheus::Registry and exposes helpers
//
// Rule of 5:
//   - Copy: DELETED — single registry per process
//   - Move: DELETED — address stability required (prometheus-cpp holds refs)
//   - Dtor: defaulted
// ---------------------------------------------------------------------------
class MetricsRegistry {
public:
    explicit MetricsRegistry(std::string_view service_name);

    MetricsRegistry(const MetricsRegistry&)            = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;
    MetricsRegistry(MetricsRegistry&&)                 = delete;
    MetricsRegistry& operator=(MetricsRegistry&&)      = delete;
    ~MetricsRegistry()                                 = default;

    // Access underlying registry for service-specific metric registration
    [[nodiscard]] prometheus::Registry& registry() noexcept { return *registry_; }

    // Pre-built common metrics
    void record_operation(std::string_view op_name,
                          std::string_view table,
                          bool             success,
                          double           duration_secs);

    void record_rows_migrated(uint64_t count);
    void record_rows_failed(uint64_t count);
    void record_rows_filtered(uint64_t count);
    void set_throughput(double rows_per_sec);

    // Expose /metrics text — called by the HTTP API handler
    [[nodiscard]] std::string serialize() const;

private:
    std::string                              service_name_;
    std::shared_ptr<prometheus::Registry>    registry_;
    prometheus::Counter*                     rows_migrated_{nullptr};
    prometheus::Counter*                     rows_failed_{nullptr};
    prometheus::Counter*                     rows_filtered_{nullptr};
    prometheus::Gauge*                       throughput_{nullptr};
};

} // namespace svckit

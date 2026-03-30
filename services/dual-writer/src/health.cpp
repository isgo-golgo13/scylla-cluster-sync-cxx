// services/dual-writer/src/health.cpp
//
// HealthChecker — verifies source + target cluster connectivity.
// C++20 port of services/dual-writer/src/health.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "svckit/database.hpp"
#include <memory>
#include <spdlog/spdlog.h>

namespace dual_writer {

// Standalone health check functions — used by API and CQL server

bool check_source_health(const std::shared_ptr<svckit::ScyllaConnection>& conn) {
    if (!conn) return false;
    const bool ok = conn->ping();
    if (!ok) spdlog::warn("Source cluster health check failed");
    return ok;
}

bool check_target_health(const std::shared_ptr<svckit::ScyllaConnection>& conn) {
    if (!conn) return false;
    const bool ok = conn->ping();
    if (!ok) spdlog::warn("Target cluster health check failed");
    return ok;
}

} // namespace dual_writer

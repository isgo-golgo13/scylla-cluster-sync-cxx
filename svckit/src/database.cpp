// svckit/src/database.cpp
//
// ScyllaConnection — Rule-of-5 RAII wrapper around DataStax C++ driver.
// C++20 port of svckit/src/database/scylla.rs
//
// Key design decisions:
//   - Copy DELETED — CassSession is a unique resource (non-shareable handle)
//   - Move defined  — transfers ownership cleanly via unique_ptr
//   - Destructor    — closes session gracefully, then releases cluster+session
//   - All CQL execution through execute() — callers wrap in Asio awaitables
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "svckit/database.hpp"
#include "svckit/errors.hpp"

#include <spdlog/spdlog.h>
#include <cassandra.h>
#include <sstream>
#include <utility>

namespace svckit {

// =============================================================================
// ScyllaConnection — Constructor
// =============================================================================

ScyllaConnection::ScyllaConnection(const DatabaseConfig& config)
    : config_{config}
    , cluster_{nullptr, CassClusterDeleter{}}
    , session_{nullptr, CassSessionDeleter{}}
    , connected_{false}
{
    connect();
}

// =============================================================================
// Rule-of-5: Move constructor
// =============================================================================

ScyllaConnection::ScyllaConnection(ScyllaConnection&& other) noexcept
    : config_{std::move(other.config_)}
    , cluster_{std::move(other.cluster_)}
    , session_{std::move(other.session_)}
    , connected_{std::exchange(other.connected_, false)}
{
}

// =============================================================================
// Rule-of-5: Move assignment
// =============================================================================

ScyllaConnection& ScyllaConnection::operator=(ScyllaConnection&& other) noexcept {
    if (this != &other) {
        close();
        config_    = std::move(other.config_);
        cluster_   = std::move(other.cluster_);
        session_   = std::move(other.session_);
        connected_ = std::exchange(other.connected_, false);
    }
    return *this;
}

// =============================================================================
// Rule-of-5: Destructor — graceful session close before handle release
// =============================================================================

ScyllaConnection::~ScyllaConnection() {
    close();
}

// =============================================================================
// connect() — establish session to cluster
// =============================================================================

void ScyllaConnection::connect() {
    spdlog::info("Connecting to {} cluster: {} hosts, keyspace={}",
                 config_.driver,
                 config_.hosts.size(),
                 config_.keyspace);

    // --- Build contact points string: "host1:port,host2:port,..."
    std::ostringstream contact_points;
    for (size_t i = 0; i < config_.hosts.size(); ++i) {
        if (i > 0) contact_points << ',';
        contact_points << config_.hosts[i];
    }

    // --- Configure cluster
    CassCluster* raw_cluster = cass_cluster_new();
    if (!raw_cluster) {
        throw DatabaseError("Failed to allocate CassCluster");
    }
    cluster_.reset(raw_cluster);

    cass_cluster_set_contact_points(cluster_.get(), contact_points.str().c_str());
    cass_cluster_set_port(cluster_.get(), static_cast<int>(config_.port));
    cass_cluster_set_connect_timeout(cluster_.get(), config_.connection_timeout_ms);
    cass_cluster_set_request_timeout(cluster_.get(), config_.request_timeout_ms);

    // Connection pool per host
    cass_cluster_set_num_threads_io(cluster_.get(),
        std::max(static_cast<unsigned>(config_.pool_size), 1u));

    // Authentication
    if (config_.username.has_value() && config_.password.has_value()) {
        cass_cluster_set_credentials(cluster_.get(),
                                     config_.username->c_str(),
                                     config_.password->c_str());
    }

    // Speculative execution for GC-pause resilience
    if (config_.speculative_execution) {
        CassSpeculativeExecutionPolicy* spec_policy =
            cass_speculative_execution_policy_constant_new(
                config_.speculative_delay_ms, /*max_executions=*/2);
        cass_cluster_set_speculative_execution_policy(cluster_.get(), spec_policy);
        cass_speculative_execution_policy_free(spec_policy);
        spdlog::info("Speculative execution enabled (delay={}ms)", config_.speculative_delay_ms);
    }

    // --- Create + connect session
    CassSession* raw_session = cass_session_new();
    if (!raw_session) {
        throw DatabaseError("Failed to allocate CassSession");
    }
    session_.reset(raw_session);

    CassFuture* connect_future = nullptr;

    if (!config_.keyspace.empty()) {
        connect_future = cass_session_connect_keyspace(
            session_.get(), cluster_.get(), config_.keyspace.c_str());
    } else {
        connect_future = cass_session_connect(session_.get(), cluster_.get());
    }

    cass_future_wait(connect_future);
    const CassError rc = cass_future_error_code(connect_future);

    if (rc != CASS_OK) {
        const char* msg = nullptr;
        size_t msg_len  = 0;
        cass_future_error_message(connect_future, &msg, &msg_len);
        const std::string error_msg{msg, msg_len};
        cass_future_free(connect_future);

        throw DatabaseError(
            std::string{"Failed to connect to "} + config_.driver +
            " cluster: " + error_msg);
    }

    cass_future_free(connect_future);
    connected_ = true;

    spdlog::info("Successfully connected to {} keyspace: {}",
                 config_.driver, config_.keyspace);
}

// =============================================================================
// close() — graceful session shutdown
// =============================================================================

void ScyllaConnection::close() noexcept {
    if (!connected_ || !session_) {
        return;
    }

    CassFuture* close_future = cass_session_close(session_.get());
    cass_future_wait(close_future);
    cass_future_free(close_future);
    connected_ = false;

    spdlog::debug("Session closed for keyspace: {}", config_.keyspace);
}

// =============================================================================
// execute() — synchronous CQL execution
//
// Service code wraps this in boost::asio::co_spawn / post() to run
// on the Asio thread pool without blocking the event loop.
// =============================================================================

void ScyllaConnection::execute(std::string_view cql) const {
    if (!connected_ || !session_) {
        throw DatabaseError("Cannot execute query — session not connected");
    }

    CassStatement* stmt = cass_statement_new_n(cql.data(), cql.size(), /*param_count=*/0);
    CassFuture* query_future = cass_session_execute(session_.get(), stmt);

    cass_future_wait(query_future);
    const CassError rc = cass_future_error_code(query_future);

    if (rc != CASS_OK) {
        const char* msg = nullptr;
        size_t msg_len  = 0;
        cass_future_error_message(query_future, &msg, &msg_len);
        const std::string error_msg{msg, msg_len};

        cass_future_free(query_future);
        cass_statement_free(stmt);

        throw DatabaseError(std::string{"Query execution failed: "} + error_msg);
    }

    cass_future_free(query_future);
    cass_statement_free(stmt);
}

// =============================================================================
// ping() — health check (SELECT now() FROM system.local)
// =============================================================================

bool ScyllaConnection::ping() const noexcept {
    if (!connected_ || !session_) {
        return false;
    }

    try {
        execute("SELECT now() FROM system.local");
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace svckit

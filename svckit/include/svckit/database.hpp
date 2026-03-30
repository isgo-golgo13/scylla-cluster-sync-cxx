#pragma once
// svckit/include/svckit/database.hpp
//
// CassandraConnection / ScyllaConnection — Rule-of-5 resource-owning wrappers
// around the DataStax C++ driver session.
// C++20 port of svckit/src/database/scylla.rs + cassandra.rs

#include "svckit/config.hpp"
#include "svckit/errors.hpp"
#include <cassandra.h>
#include <memory>
#include <string>
#include <string_view>

namespace svckit {

// ---------------------------------------------------------------------------
// Custom deleter for CassSession (RAII wrapper around raw C handle)
// ---------------------------------------------------------------------------
struct CassSessionDeleter {
    void operator()(CassSession* s) const noexcept {
        if (s) {
            cass_session_free(s);
        }
    }
};

struct CassClusterDeleter {
    void operator()(CassCluster* c) const noexcept {
        if (c) {
            cass_cluster_free(c);
        }
    }
};

// ---------------------------------------------------------------------------
// ScyllaConnection — owns a CassSession + CassCluster
//
// Rule of 5:
//   - Copy: DELETED — connections are unique resources
//   - Move: defined  — transfers ownership of session/cluster handles
//   - Dtor: defined  — closes session before freeing handles
// ---------------------------------------------------------------------------
class ScyllaConnection {
public:
    explicit ScyllaConnection(const DatabaseConfig& config);

    // Rule of 5
    ScyllaConnection(const ScyllaConnection&)            = delete;
    ScyllaConnection& operator=(const ScyllaConnection&) = delete;
    ScyllaConnection(ScyllaConnection&&) noexcept;
    ScyllaConnection& operator=(ScyllaConnection&&) noexcept;
    ~ScyllaConnection();

    // Accessors
    [[nodiscard]] CassSession* session() const noexcept { return session_.get(); }
    [[nodiscard]] bool         is_connected() const noexcept { return connected_; }
    [[nodiscard]] std::string  keyspace() const { return config_.keyspace; }

    // Execute a CQL query (synchronous — wrap in Asio awaitable at call site)
    void execute(std::string_view cql) const;

    // Health check
    [[nodiscard]] bool ping() const noexcept;

private:
    DatabaseConfig                          config_;
    std::unique_ptr<CassCluster, CassClusterDeleter> cluster_;
    std::unique_ptr<CassSession, CassSessionDeleter> session_;
    bool                                    connected_{false};

    void connect();
    void close() noexcept;
};

// ---------------------------------------------------------------------------
// CassandraConnection — alias for now; diverges when driver-level
// differences between Cassandra and ScyllaDB need separate handling
// ---------------------------------------------------------------------------
using CassandraConnection = ScyllaConnection;

} // namespace svckit

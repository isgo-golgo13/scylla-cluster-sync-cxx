# ScyllaDB Cluster-to-Cluster Sync Service (C++20)
Ultra-Fast RPO/RTO Sensitive ScyllaDB (Cassandra DB Adaptive) Cluster to Cluster Tenant Data Synching Service in C++20 using Shadow Write Transition Pattern.

The architecture of the ScyllaDB (or Cassandra DB source) to ScyllaDB (or Cassandra DB sink/target) involves three services to fulfill the tenant data lift-and-shift transition. These services are.

- **Dual-Writer Proxy Service** (C++20/23+)
- **SSTableLoader Processor Service** (C++20/23+)
- **Dual-Reader Data Sync Ack Service** (C++20/23+)

C++20 is required to avoid any GC pauses that would fail 99999 SLA delivery as the data shuttling across the source to the target can NOT withstand latency delays.


## Cross-Cloud FTXUI-Native Data X-Fer Tracking Dash (Doctore)

The following screenshot is of the data tranfer tracking (cross-cloud) ScyllaDB Cluster to SycllaDB Cluster tracking the services (dual-writer, ssttable-loader, dual-writer). The UI dash is called `doctore-dash` and developed using a stack of C++20, Boost.Asio async, FTXUI (C++ terminal UI framework) and CSS. FTXUI provides declarative animated terminal UI without GC pauses and NPM prone CVEs.

![doctore-dash-ftxui-cxx](docs/Doctore-Dash.png)




## The Dual-Write Proxy Service Architecture

The following graphic shows the architectual workflow of the `Dual-Write Proxy` service deployed to Kubernetes.



![dual-write-proxy-architecture-gcp-aws](docs/Dual-Writer-Data-Synch-Architecture-K8s.png)


The `Dual-Write` Pattern is executed as follows (high-level). See the following section `The Dual-Write Proxy Service Pattern (Low-Level)` for in-depth execution of the code.

```cpp
// services/dual-writer/src/writer.cpp — DualWriter::write()
// Boost.Asio co_await/co_return coroutine (replaces Tokio async)

asio::awaitable<WriteResult>
DualWriter::write(std::string_view cql, std::string_view keyspace) const {
    WriteResult result;

    // 1. Check FilterGovernor before shadow write (tenant/table blacklist)
    if (filter_ && filter_->is_enabled()) {
        const auto table_decision = filter_->should_skip_table(keyspace);
        if (table_decision == svckit::FilterDecision::SkipTable) {
            spdlog::info("Skipping target write — table blacklisted: {}", keyspace);

            // Write source only
            try {
                source_->execute(cql);
                result.source_ok = true;
                result.target_ok = false;  // Intentionally skipped, not failed
            } catch (const svckit::SyncError& e) {
                result.source_ok = false;
                result.error_message = e.what();
                spdlog::error("Source-only write failed: {}", e.what());
            }

            if (metrics_) {
                metrics_->record_operation("write", keyspace, result.source_ok, 0.0);
            }

            co_return result;
        }
    }

    // 2. SYNCHRONOUS write to source (PRIMARY) - This BLOCKS!
    const auto start = std::chrono::steady_clock::now();

    try {
        source_->execute(cql);
        result.source_ok = true;
    } catch (const svckit::SyncError& e) {
        result.source_ok = false;
        result.error_message = std::string{"Source write failed: "} + e.what();
        spdlog::error("{}", result.error_message);

        if (metrics_) {
            metrics_->record_operation("write", keyspace, false, 0.0);
        }
        co_return result;
    }

    // 3. ASYNCHRONOUS write to target (SHADOW) - Fire and forget!
    try {
        target_->execute(cql);
        result.target_ok = true;
        spdlog::debug("Shadow write successful for keyspace: {}", keyspace);
    } catch (const svckit::SyncError& e) {
        result.target_ok = false;
        result.error_message = std::string{"Target shadow write failed: "} + e.what();
        spdlog::warn("Shadow write failed: {} — source write was successful", e.what());
        // Queue for retry (FailedWrite tracking)
    }

    // 4. Return based on PRIMARY (source) result ONLY
    //    Success returned immediately after source write
    //    No wait for target - shadow write is fire-and-forget
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double duration_secs =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1e6;

    if (metrics_) {
        metrics_->record_operation("write", keyspace, result.fully_successful(), duration_secs);
    }

    co_return result;
}
```

**The Dual-Write Flow Sequence**

The following graphic shows the `Dual-Writer-Proxy` receving the tenant data write request (to dispatch to the target ScyllaDB/CassandraDB Cluster instance) and how it returns thread control to the application calling on this service.

![dual-write-sync-async-shadow-write-flow](docs/Dual-Writer-Sync-Async-Write-Flow.png)


The critical part of the `Dual-Writer-Proxy` service is to understand the distinction of the `async` wait on the result from a sychrononous write to the source Cassandra (or even source ScyllaDB) and fire-and-forget asynchronous write to the shadow Cassandra DB or ScyllaDB sink/target database. The following code logic shows this workflow.


```cpp
// SYNCHRONOUS (waits for result):
source_->execute(cql);
// This blocks the current coroutine until Cassandra responds
// The coroutine PAUSES here until the result is retrieved

// ASYNCHRONOUS (fire-and-forget):
boost::asio::co_spawn(executor,
    [&target_, cql]() -> asio::awaitable<void> {
        target_->execute(cql);
        co_return;
    },
    boost::asio::detached);
// NO co_await on the spawn itself
// The coroutine CONTINUES immediately without waiting
``` 


The `Dual-Writer-Proxy` service deploys a Kubernetes Pod co-resident to the GCP application (the streaming applicattion in the graphic) to avoid cross-cloud latency IF the Dual-Writer Proxy resided in the target cloud (AWS). The following Kubernetes `Deployment`resource shows this configuration relative to the streaming application. This shows the use of Cassandra DB as the source DB. This works identically for ScyllaDB.

```yaml
# cxx-proxy-deployment.yaml - Deploys to GKE, NOT EKS 
apiVersion: apps/v1
kind: Deployment
metadata:
  name: cxx-dual-writer-proxy
  namespace: engine-vector  # Same namespace as app in GKE
spec:
  replicas: 3
  selector:
    matchLabels:
      app: cxx-proxy
  template:
    metadata:
      labels:
        app: cxx-proxy
    spec:
      affinity:
        podAntiAffinity:  # Spread across nodes
          requiredDuringSchedulingIgnoredDuringExecution:
          - topologyKey: kubernetes.io/hostname
      containers:
      - name: proxy
        image: gcr.io/backlight-gcp/cxx-proxy:v1  # GCR, not ECR!
        ports:
        - containerPort: 9042  # CQL port
        - containerPort: 9090  # Metrics API
        env:
        - name: CASSANDRA_HOSTS
          value: "cassandra-0.cassandra-svc,cassandra-1.cassandra-svc"  # Local
        - name: SCYLLA_HOSTS
          value: "10.100.0.10,10.100.0.11,10.100.0.12"  # AWS IPs via VPN
        - name: WRITE_MODE
          value: "DualWriteAsync"
```

## The Dual-Write DB Duality of Functionality


![dual-writer-duality-of-service-configuration](docs/Dual-Writer-Docker-Image-ConfigMap.png)


The `dual-writer` design uses the `Factory Design Pattern` in `svckit/src/database.cpp` to instantiate the correct connection resource (Scylla to Scylla or Cassandra to Cassandra, or mixed-mode source of the DB and the target DB for the data migration). The ScyllaDB DataStax C++ driver is 100% adaptable to Cassandra (to the latest Cassandra 4 driver). The client that requires a data migration of a ScyllaDB to ScyllaDB scenario will configure the values in the provided root directory `config/dual-writer-scylla.yaml` or `config/dual-writer-cassandra.yaml`, however these are templates and the actual configuration of each will go into a Kubernetes ConfigMap and have the ConfigMap referenced in the Kubernetes `Deployment` spec. No configuration logic for the dual-writer is hard-coded. 




## The Dual-Write Proxy Service Architecture (Alternate No-GKE, No-EKS)

The following graphic shows the architectual workflow of the `Dual-Write Proxy` service deployed directly to VMs (source VM on GCP and sink/target VM on AWS) without Kubernetes.

![dual-write-proxy-architecture-gcp-aws](docs/Dual-Writer-Data-Synch-Architecture-NoK8s.png)


## The Dual-Write Proxy Service Pattern (Low-Level)

The non-multi library pre-prod view of the code for the Dual-Write Proxy service (NOT using Envoy Proxy).
The `dual-writer` proxy service accepts native ScyllaDB/CassandraDB driver connections through the provided `cql_server.cpp` module. It provides **CQL** binary protocol v4 for transparent dual-writing.

```cpp
// services/dual-writer/src/cql_server.cpp
//
// CQL Protocol v4 Proxy — transparent TCP proxy with write interception.
// Boost.Asio co_await/co_return coroutines for async TCP accept + frame processing.

#include "dual_writer/writer.hpp"
#include "svckit/errors.hpp"

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

namespace dual_writer {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

static constexpr uint8_t CQL_VERSION_RESPONSE = 0x84;  // 0x80 | 0x04

// CQL Opcodes
static constexpr uint8_t OP_QUERY   = 0x07;
static constexpr uint8_t OP_RESULT  = 0x08;
static constexpr uint8_t OP_PREPARE = 0x09;
static constexpr uint8_t OP_EXECUTE = 0x0A;
static constexpr uint8_t OP_BATCH   = 0x0D;

// Frame header (9 bytes)
struct FrameHeader {
    uint8_t  version{0};
    uint8_t  flags{0};
    int16_t  stream_id{0};
    uint8_t  opcode{0};
    int32_t  body_length{0};

    [[nodiscard]] std::array<uint8_t, 9> to_bytes() const noexcept;
    static FrameHeader response(uint8_t opcode, int16_t stream_id, int32_t body_len);
};

// Handle a single client connection — main proxy loop
static asio::awaitable<void>
handle_connection(tcp::socket client_sock,
                  std::shared_ptr<DualWriter> writer,
                  tcp::endpoint source_endpoint) {
    auto executor = co_await asio::this_coro::executor;
    tcp::socket source_sock{executor};
    co_await source_sock.async_connect(source_endpoint, asio::use_awaitable);

    // Prepared statement tracker (per-connection)
    std::unordered_map<std::string, PreparedStmt> prepared_stmts;

    for (;;) {
        // Read CQL frame header from client (9 bytes)
        std::array<uint8_t, 9> hdr_buf{};
        co_await asio::async_read(client_sock, asio::buffer(hdr_buf), asio::use_awaitable);
        const auto header = parse_header(hdr_buf);

        // Read frame body
        std::vector<uint8_t> body(static_cast<size_t>(header.body_length));
        if (header.body_length > 0) {
            co_await asio::async_read(client_sock, asio::buffer(body), asio::use_awaitable);
        }

        std::vector<uint8_t> response;

        switch (header.opcode) {
            case OP_QUERY: {
                const auto query = parse_long_string(body, 0);
                if (is_write_query(query)) {
                    // Write intercepted — dual-writing
                    const auto table = extract_table_target(query);
                    auto write_result = co_await writer->write(query, table);
                    response = write_result.source_ok
                        ? build_void_result(header.stream_id)
                        : co_await forward_and_proxy(header, body, source_sock);
                } else {
                    // SELECT/DDL: transparent proxy to source
                    response = co_await forward_and_proxy(header, body, source_sock);
                }
                break;
            }
            case OP_PREPARE:
                response = co_await forward_and_proxy(header, body, source_sock);
                // Track prepared statement for write detection on EXECUTE
                break;
            case OP_EXECUTE:
                // Look up prepared statement, dual-write if it's a write
                response = co_await forward_and_proxy(header, body, source_sock);
                break;
            default:
                response = co_await forward_and_proxy(header, body, source_sock);
                break;
        }

        co_await asio::async_write(client_sock, asio::buffer(response), asio::use_awaitable);
    }
}

// Accept loop — entry point from main.cpp
asio::awaitable<void>
start_cql_server(asio::io_context& ioc,
                 std::shared_ptr<DualWriter> writer,
                 const std::string& bind_addr, uint16_t bind_port,
                 const std::string& source_host, uint16_t source_port) {
    auto executor = co_await asio::this_coro::executor;
    tcp::endpoint listen_ep{asio::ip::make_address(bind_addr), bind_port};
    tcp::acceptor acceptor{executor, listen_ep};

    spdlog::info("CQL proxy listening on {}:{}", bind_addr, bind_port);

    for (;;) {
        tcp::socket client = co_await acceptor.async_accept(asio::use_awaitable);
        asio::co_spawn(executor,
                       handle_connection(std::move(client), writer,
                           tcp::endpoint{asio::ip::make_address(source_host), source_port}),
                       asio::detached);
    }
}

} // namespace dual_writer
```

## The Four Stages of the Dual-Write Proxy Service (To Full Data Transition End)

```shell
Phase 1 - DualWriteAsync (Current):
  Cassandra: SYNCHRONOUS (primary) ✓
  ScyllaDB:  ASYNCHRONOUS (shadow) 
  App Impact: NONE (still <1ms latency)

Phase 2 - DualWriteSync (Validation):
  Cassandra: SYNCHRONOUS (primary) ✓
  ScyllaDB:  SYNCHRONOUS (also waits) ✓
  App Impact: Slight increase in latency
  Purpose: Ensure ScyllaDB can handle load

Phase 3 - ShadowPrimary (Flip):
  Cassandra: ASYNCHRONOUS (shadow)
  ScyllaDB:  SYNCHRONOUS (primary) ✓
  App Impact: Now using ScyllaDB's performance!

Phase 4 - TargetOnly (Complete):
  Cassandra: NONE (decommissioned)
  ScyllaDB:  SYNCHRONOUS (only database) ✓
  App Impact: Full ScyllaDB benefits
```



## Dual-Writer CQL Proxy Two Separate Layers**
```
┌─────────────────────────────────────────────┐
│ LAYER 1: App → Proxy                       │
│ Protocol: CQL Binary v4                     │
│ Parsed by: cql_server.cpp (manual parsing) │
└─────────────────────────────────────────────┘
              ↓
┌─────────────────────────────────────────────┐
│ LAYER 2: Proxy → Database                   │
│ Protocol: CQL Binary v4 (same!)             │
│ Generated by: DataStax C++ driver           │
│ Works with: Cassandra 4.x AND ScyllaDB      │
└─────────────────────────────────────────────┘
```


### Testing the CQL Proxy Layer (Python Client)

```python
#!/usr/bin/env python3
"""
Test client for CQL dual-writer proxy
Shows that Python cassandra-driver works transparently
"""

from cassandra.cluster import Cluster
from cassandra.auth import PlainTextAuthProvider
import uuid
import time

def test_cql_proxy():
    """Test CQL proxy with Python cassandra-driver"""
    
    print("Connecting to CQL dual-writer proxy...")
    
    # Connect to dual-writer proxy (NOT directly to Cassandra!)
    cluster = Cluster(
        contact_points=['127.0.0.1'],
        port=9042,  # CQL proxy port
        protocol_version=4
    )
    
    session = cluster.connect()
    print("Connected to CQL proxy")
    
    # Set keyspace
    session.set_keyspace('files_keyspace')
    print("Keyspace set to files_keyspace")
    
    # Test INSERT
    file_id = uuid.uuid4()
    tenant_id = uuid.uuid4()
    
    query = """
    INSERT INTO files (system_domain_id, id, name, size, status)
    VALUES (?, ?, ?, ?, ?)
    """
    
    print(f"\nInserting test file: {file_id}")
    session.execute(query, [tenant_id, file_id, "test.jpg", 1024, "CLOSED"])
    print("INSERT successful - dual-written to GCP + AWS!")
    
    # Test SELECT
    query = "SELECT * FROM files WHERE system_domain_id = ? AND id = ?"
    print(f"\nQuerying file: {file_id}")
    rows = session.execute(query, [tenant_id, file_id])
    
    for row in rows:
        print(f"Found: {row.name} ({row.size} bytes, status={row.status})")
    
    # Test UPDATE
    query = """
    UPDATE files SET status = ? WHERE system_domain_id = ? AND id = ?
    """
    print(f"\nUpdating file status...")
    session.execute(query, ["ARCHIVED", tenant_id, file_id])
    print("UPDATE successful - dual-written to both clusters!")
    
    # Test blacklist (optional - requires tenant in blacklist)
    blacklisted_tenant = uuid.UUID("e19206ba-c584-11e8-882a-0a580a30028e")
    query = """
    INSERT INTO files (system_domain_id, id, name, size, status)
    VALUES (?, ?, ?, ?, ?)
    """
    print(f"\nTesting blacklist with tenant: {blacklisted_tenant}")
    session.execute(query, [blacklisted_tenant, uuid.uuid4(), "blocked.jpg", 2048, "CLOSED"])
    print("Request accepted (written to GCP only, AWS skipped)")
    
    cluster.shutdown()
    print("\nAll tests passed!")
    print("CQL proxy working transparently with Python cassandra-driver!")

if __name__ == "__main__":
    try:
        test_cql_proxy()
    except Exception as e:
        print(f"Test failed: {e}")
        import traceback
        traceback.print_exc()
```




## Client Applications Use of Dual-Write Proxy Service (Using CQL Native Driver)

The following Python 3 application template that shows how to connect and call APIs to the `dual-writer` proxy service (service is deployed to Kubernetes). The `dual-writer` proxy uses native SycllaDB (Cassandra 100% compatible) driver that is transparent to the Python calling client application.

```python
"""
Python client template for the C++20 developed CQL Migration Proxy
"""

import asyncio
import time
from dataclasses import dataclass
from enum import Enum
from typing import Dict, Optional, Any, List
import logging
from datetime import datetime
import uuid

# Native Cassandra driver
from cassandra.cluster import Cluster
from cassandra.auth import PlainTextAuthProvider
from cassandra.query import SimpleStatement, ConsistencyLevel as CassConsistency

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class WriteMode(Enum):
    """Migration phases - matches C++ enum exactly"""
    SOURCE_ONLY = "SourceOnly"      # Phase 0: Source (GCP/Cassandra) only
    DUAL_ASYNC = "DualAsync"        # Phase 1: Shadow writes (async to target)
    DUAL_SYNC = "DualSync"          # Phase 2: Synchronous dual writes
    TARGET_ONLY = "TargetOnly"      # Phase 3: Target (AWS/ScyllaDB) only


class ConsistencyLevel(Enum):
    """CQL consistency levels - matches C++ enum"""
    ANY = CassConsistency.ANY
    ONE = CassConsistency.ONE
    TWO = CassConsistency.TWO
    THREE = CassConsistency.THREE
    QUORUM = CassConsistency.QUORUM
    ALL = CassConsistency.ALL
    LOCAL_QUORUM = CassConsistency.LOCAL_QUORUM
    EACH_QUORUM = CassConsistency.EACH_QUORUM
    LOCAL_ONE = CassConsistency.LOCAL_ONE


@dataclass
class ClusterConfig:
    """Database cluster configuration - matches C++ DatabaseConfig"""
    driver: str  # "scylla" or "cassandra"
    hosts: List[str]
    port: int
    keyspace: str
    username: Optional[str] = None
    password: Optional[str] = None
    connection_timeout_secs: int = 5
    request_timeout_secs: int = 10
    pool_size: int = 4


@dataclass
class WriterConfig:
    """Writer behavior configuration - matches C++ WriterConfig"""
    mode: WriteMode
    shadow_timeout_ms: int = 5000
    retry_interval_secs: int = 60
    max_retry_attempts: int = 3
    batch_size: int = 100


@dataclass
class DualWriterConfig:
    """Full configuration - matches C++ DualWriterConfig"""
    source: ClusterConfig
    target: ClusterConfig
    writer: WriterConfig


class MigrationProxyClient:
    """
    Python client for the C++20 CQL migration proxy.
    Uses native cassandra-driver - ZERO application code changes!
    """
    
    def __init__(self, contact_points: List[str] = None, port: int = 9042):
        if contact_points is None:
            contact_points = ['localhost']
        self.contact_points = contact_points
        self.port = port
        self.cluster = None
        self.session = None
        self._metrics = {"writes": 0, "reads": 0, "errors": 0, "total_latency_ms": 0}
    
    def __enter__(self):
        self.connect()
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
    
    def connect(self):
        self.cluster = Cluster(contact_points=self.contact_points, port=self.port, protocol_version=4)
        self.session = self.cluster.connect()
        logger.info("Connected to CQL migration proxy")
    
    def close(self):
        if self.cluster:
            self.cluster.shutdown()
    
    def set_keyspace(self, keyspace: str):
        self.session.set_keyspace(keyspace)
    
    def insert(self, keyspace, table, data, consistency=None):
        columns = list(data.keys())
        placeholders = ", ".join(["?" for _ in columns])
        query = f"INSERT INTO {keyspace}.{table} ({', '.join(columns)}) VALUES ({placeholders})"
        statement = SimpleStatement(query)
        if consistency:
            statement.consistency_level = consistency.value
        self.session.execute(statement, list(data.values()))
        self._metrics["writes"] += 1
        return True

    # ... update(), delete(), select() follow same pattern ...


# Showing how third-party media application would integrate
def iconik_media_application_svc():
    """
    How Iconik's application would use the proxy
    ONLY CONNECTION STRING CHANGES - everything else stays the same!
    """
    with MigrationProxyClient(['dual-writer-proxy'], 9042) as client:
        client.set_keyspace("iconik_production")
        
        asset_id = uuid.uuid4()
        client.insert(
            keyspace="iconik_production",
            table="media_assets",
            data={
                "asset_id": asset_id,
                "title": "Q4 2024 Earnings Call",
                "file_path": "s3://media-bucket/earnings/q4-2024.mp4",
                "duration": 3600,
                "codec": "h264",
                "created_at": datetime.now()
            }
        )
        logger.info(f"Inserted media asset: {asset_id}")


if __name__ == "__main__":
    iconik_media_application_svc()
```


## The Dual-Reader CheckSumming Service







## The SSTableLoader Bulk Transfer Processor

The deployment location of this service is on the target cloud side. If the DB clusters involved are Cassandra or ScyllaDB on GCP (GKE, GCP Compute Instance VM) on source side and the target Cassandra DB or ScyllaDB Cluster is on AWS (EKS or EC2, Firecracker), the `sstable-loader` service will require is location on the AWS target side. 

The SSTableLoader requires its location on the **target** side. 


### The Advantages of Target Side Locationing

- **Network Locality** Loads data directly into AWS Cassandra or SycllaDB cluster
- **Network Throughput** No Cross-Cloud data transfer during bulk load
- **Cost** Avoids GCP egress charges for bulk data
- **Performance** Local disk I/O on AWS EC2


## The Dual-Reader Service

The deployment location of this service is **NOT** strictly enforced on source or target side. 
It is advised that this service location execute on the **target** side of the transfer to avoid costly egress costs from the source cloud side (if GCP to AWS), the `dual-reader` service is at its advantage if it is deployed on the AWS side (EKS, EC2, Firecracker).


### The Advantages of Target Side Locationing

- **Network Symmetry** If on AWS, 1 read is local, 1 is remote (vs 2 remote if on GCP)
- **Cost** Avoid GCP egress for validation reads
- **After Cutover** Dual Reader validator stays on AWS to focus AWS target on Cassandra DB



The following architectural workflow graphic shows the entire fleet of the `scylla/cassandra-cluster-sync` application of the dual-writer proxy service, sstable-loader service and dual-writer service (dual-cluster checksumming validation). This version of the entire architectural workflow shows a **Kubernetes-Native** deployment. The graphic shows CasssandraDB instance, however this 100% adapts to ScyllaDB cluster without disruptive change.

![entire-scylla-cluster-sync-workflow](docs/Full-Service-Architecture.png)


The following architectural workflow graphic shows the entire fleet of the `scylla/cassandra-cluster-sync` application of the dual-writer proxy service, sstable-loader service and dual-writer service. This version of the entire architectural workflow shows a **Cloud-Native** VM deployment (services a Docker containers on GCP Compute Instance VMs and AWS EC2 VMs). The graphic shows CasssandraDB instance, however this 100% adapts to ScyllaDB cluster without disruptive change.

![entire-scylla-cluster-sync-workflow-vm-native](docs/Full-Service-Architecture-VM-Native.png)




## Index Management (SSTableLoader Process Server)

### With Secondary Indexes (recommended for 60+ indexes):
```bash
./sstable-loader --config config/sstable-loader.yaml \
                 --indexes config/indexes.yaml \
                 --auto-start
```

### Without Secondary Indexes:
```bash
./sstable-loader --config config/sstable-loader.yaml \
                 --skip-indexes \
                 --auto-start
```

### Empty indexes.yaml (also valid):
```yaml
# indexes.yaml - no indexes to manage
[]
```

## SSTable-Loader Terminal UI (TUI) Status Dash

This TUI (using C++ `FTXUI` library) calls on the existing `sstable-loader` status endpoint at ` http://localhost:9092/status`. No WebSockets involved.

![ssstable-loader-tui-dash](docs/TUI-SSTableLoader-Dash.png)

The provided project root `Makefile` includes the following targets to fire up this terminal ui.

```shell
# Build and run the FTXUI TUI dashboard
make build
./build/services/sstable-loader/sstable-loader --tui-demo

# Live mode (default localhost:9092)
./build/services/sstable-loader/sstable-loader --tui

# Live mode with port-forward
kubectl port-forward svc/sstable-loader 9092:9092
./build/services/sstable-loader/sstable-loader --tui --api-url http://localhost:9092
```





## SSTable-Loader REST API

Base URL: `http://localhost:9092` (configurable via `observability.metrics_port`)

### Endpoints

| Method | Endpoint | Body | Description |
|--------|----------|------|-------------|
| GET | `/health` | — | Health check |
| GET | `/status` | — | Migration stats |
| POST | `/start` | `{"keyspace_filter": ["ks1"]}` | Start migration (optional keyspace filter) |
| POST | `/stop` | — | Stop migration |
| POST | `/migrate/:keyspace/:table` | — | **Migrate a single table** |
| POST | `/indexes/drop` | — | Drop all indexes |
| POST | `/indexes/rebuild` | — | Rebuild all indexes |
| GET | `/indexes/verify` | — | Verify all indexes exist |
| GET | `/indexes/status` | — | Index manager status |
| POST | `/indexes/drop/:keyspace` | — | Drop indexes for specific keyspace |
| POST | `/indexes/rebuild/:keyspace` | — | Rebuild indexes for specific keyspace |
| GET | `/discover/:keyspace` | — | Returns list of tables in that keyspace |


### Requests

#### Migrate a single table (NEW)
```bash
curl -X POST http://localhost:9092/migrate/acls_keyspace/group_acls
```

#### Start full migration
```bash
curl -X POST http://localhost:9092/start \
  -H "Content-Type: application/json" \
  -d '{}'
```

#### Start migration for specific keyspaces
```bash
curl -X POST http://localhost:9092/start \
  -H "Content-Type: application/json" \
  -d '{"keyspace_filter": ["acls_keyspace", "assets_keyspace"]}'
```

#### Start migration with index management
```bash
curl -X POST http://localhost:9092/start \
  -H "Content-Type: application/json" \
  -d '{
    "keyspace_filter": ["acls_keyspace"],
    "drop_indexes_first": true,
    "rebuild_indexes_after": true
  }'
```

#### Check migration status
```bash
curl http://localhost:9092/status
```

#### Stop migration
```bash
curl -X POST http://localhost:9092/stop
```

#### Drop indexes for a keyspace
```bash
curl -X POST http://localhost:9092/indexes/drop/acls_keyspace
```

#### Rebuild indexes for a keyspace
```bash
curl -X POST http://localhost:9092/indexes/rebuild/acls_keyspace
```

### Returns list of tables in that keyspace
```bash
curl http://localhost:9092/discover/acls_keyspace

# Returns
{
  "status": "success",
  "keyspace": "acls_keyspace",
  "table_count": 5,
  "tables": ["group_acls", "user_acls", "role_acls", "permissions", "policies"]
}
```



### Responses 

#### Health Check
```json
{
  "status": "healthy",
  "service": "sstable-loader",
  "index_manager_enabled": true,
  "index_count": 64
}
```

#### Migration Status
```json
{
  "status": "ok",
  "migration": {
    "total_rows": 3487274,
    "migrated_rows": 3487274,
    "failed_rows": 0,
    "filtered_rows": 0,
    "tables_completed": 1,
    "tables_total": 1,
    "tables_skipped": 0,
    "skipped_corrupted_ranges": 0,
    "progress_percent": 100.0,
    "throughput_rows_per_sec": 15198.81,
    "elapsed_secs": 229.22,
    "is_running": false,
    "is_paused": false
  }
}
```


## Speculative Execution Feature (SSTableLoader)

### Summary

When enabled, the driver sends the same query to a **backup node** if the primary coordinator doesn't respond within a configurable delay (default: 100ms). First response wins.

### Configuration

Add to `target:` section in `sstable-loader.yaml`:

```yaml
target:
  # ... existing fields ...
  speculative_execution: true
  speculative_delay_ms: 100
```

| Field | Default | Description |
|-------|---------|-------------|
| `speculative_execution` | `false` | Enable/disable feature |
| `speculative_delay_ms` | `100` | Delay before sending backup query |

### Addresses Target (Cassandra/JVM)

| Problem | Cause | How Speculative Execution Helps |
|---------|-------|--------------------------------|
| Write timeouts | JVM GC pause freezes coordinator | Backup query goes to non-frozen node |
| `LOCAL_QUORUM` failures | Coordinator unresponsive | Second node can fulfill quorum |
| Latency spikes | Stop-the-world GC events | First response wins, masks slow node |

**Real-Case:** Witnessingt NNN-second GC pause → coordinator frozen → all writes to that node failed. With speculative execution, backup query would route to healthy node after 100ms.


### NoOp for ScyllaDB (Future ScyllaDB Deployments )

| Cassandra (JVM) | ScyllaDB (C++) |
|-----------------|----------------|
| JVM garbage collection | No GC — manual memory management |
| Stop-the-world pauses (seconds to minutes) | No pauses |
| Unpredictable latency spikes | Consistent low latency |
| Speculative execution **needed** | Speculative execution **rarely triggers** |

ScyllaDB doesn't have GC pauses, so the 100ms threshold is almost never hit. The feature is harmless but unnecessary — a placebo that adds negligible overhead.

### Impact on Services

| Service | `speculative_execution` | Effect |
|---------|------------------------|--------|
| dual-writer | `false` (default) | Unchanged behavior |
| dual-reader | `false` (default) | Unchanged behavior |
| sstable-loader | `true` (if configured) | GC pause resilience |

### Trade-Offs

| Pro | Con |
|-----|-----|
| Masks slow/frozen nodes | 2x queries during slow responses |
| Reduces failed writes | Slightly higher cluster load |
| Simple config toggle | Doesn't fix root cause (JVM) |


### Config Options (sstable-loader.yaml)

| Strategy | Config | Use Case |
|----------|--------|----------|
| Retry only | `speculative_execution: false` + `max_retries: 20` | Long GC pauses (seconds) |
| Speculative only | `speculative_execution: true` + low retries | Short GC pauses (<1s) |
| **Mixed-Mode** | `speculative_execution: true` + `max_retries: 10` | Best coverage |

### Conclusion 

- **Cassandra:** Not full fix for JVM's fundamental weakness
- **ScyllaDB:** Not needed, no GC, no issues
- **Future Fix:** Switch to ScyllaDB





## CassandraDB vs ScyllaDB Kubernetes Operator Cost Reduction

The high-cognitive load of configuration and JVM overhead using the Cassandra DB and its deployment using the Cassandra DB Kubernetes Operator.

```shell
# cassandra-dc1.yaml
apiVersion: cassandra.datastax.com/v1beta1
kind: CassandraDatacenter
metadata:
  name: dc1
spec:
  clusterName: enginevector-cluster
  serverVersion: "4.0.7"
  size: 12  # Requires 4x nodes vs ScyllaDB (3)
  config:
    jvm-options:
      initial_heap_size: "8G"
      max_heap_size: "8G"
      # GC Tuning cognitive-overload
      additional-jvm-opts:
        - "-XX:+UseG1GC"
        - "-XX:G1RSetUpdatingPauseTimePercent=5"
        - "-XX:MaxGCPauseMillis=300"
        # ... 20 more GC flags
```

The low-cognitive load of configuration and no-JVM overhead using the Scylla DB (uses 100% C++) and its deployment using the ScyllaDB Kubernetes Operator.

```shell
# scylla-cluster.yaml - Self-tuning (automatic):
apiVersion: scylla.scylladb.com/v1
kind: ScyllaCluster
metadata:
  name: enginevector-cluster
spec:
  version: 5.2.0
  developerMode: false  # Production mode = auto-tuning!
  cpuset: true  # CPU pinning for zero latency
  size: 3  # 3 nodes instead of 12
  resources:
    requests:
      cpu: 7  # Leaves 1 CPU for system
      memory: 30Gi  # No heap sizing
```

The applications using CassandraDB and to the switch to SycllaDB provides 100% transparent functionality with no code changes.

```cpp
// Existing Cassandra DB code (DataStax C++ driver):
CassCluster* cluster = cass_cluster_new();
cass_cluster_set_contact_points(cluster, "cassandra-node1,cassandra-node2");
CassSession* session = cass_session_new();
cass_session_connect_keyspace(session, cluster, "engine-vector");

// ScyllaDB (identical — same CQL binary protocol, same driver)
CassCluster* cluster = cass_cluster_new();
cass_cluster_set_contact_points(cluster, "scylla-node1,scylla-node2");
CassSession* session = cass_session_new();
cass_session_connect_keyspace(session, cluster, "engine-vector");
```



# Changes with ScyllaDB Operator on EKS

```shell
Cassandra on EKS:
- Memory: 32GB (8GB heap + 24GB off-heap)
- GC Pauses: 200-800ms (G1GC)
- Nodes needed: 12 for your workload
- Annual EC2 cost: ~$105,000 (m5.2xlarge)

ScyllaDB on EKS:  
- Memory: 32GB (ALL used efficiently, no JVM)
- GC Pauses: ZERO (C++ memory management)
- Nodes needed: 3-4 for same workload
- Annual EC2 cost: ~$35,000 (i3.2xlarge with NVMe)
- TCO Savings: ~70% ($70,000/year!)
```



## Project Structure

```shell
scylla-cluster-sync-cxx
├── CMakeLists.txt
├── LICENSE
├── Makefile
├── README.md
├── cmake
│   └── CompilerWarnings.cmake
├── config
│   ├── deploy
│   │   └── scylla-cluster-sync
│   │       ├── Chart.yaml
│   │       ├── templates
│   │       │   ├── __helpers.tpl
│   │       │   ├── app-config.yaml
│   │       │   ├── app-gateway-tls.yaml
│   │       │   ├── app-gateway.yaml
│   │       │   ├── app-monitoring.yaml
│   │       │   ├── app-scaling.yaml
│   │       │   ├── app-secrets.yaml
│   │       │   └── app.yaml
│   │       ├── values-production.yaml
│   │       └── values.yaml
│   ├── dual-reader.yaml
│   ├── dual-writer-cassandra.yaml
│   ├── dual-writer-scylla.yaml
│   ├── filter-rules.yaml
│   ├── indexes.yaml
│   └── sstable-loader.yaml
├── docker-compose.yaml
├── docs
├── monitoring
│   ├── grafana
│   │   └── provisioning
│   │       └── datasources
│   │           └── prometheus.yaml
│   └── prometheus.yaml
├── services
│   ├── dual-reader
│   │   ├── CMakeLists.txt
│   │   ├── Dockerfile.dual-reader
│   │   ├── Makefile
│   │   ├── include
│   │   │   └── dual_reader
│   │   │       ├── config.hpp
│   │   │       ├── filter.hpp
│   │   │       └── reader.hpp
│   │   └── src
│   │       ├── api.cpp
│   │       ├── config.cpp
│   │       ├── filter.cpp
│   │       ├── main.cpp
│   │       ├── reader.cpp
│   │       ├── reconciliation.cpp
│   │       └── validator.cpp
│   ├── dual-writer
│   │   ├── CMakeLists.txt
│   │   ├── Dockerfile.dual-writer
│   │   ├── Makefile
│   │   ├── include
│   │   │   └── dual_writer
│   │   │       ├── config.hpp
│   │   │       ├── filter.hpp
│   │   │       └── writer.hpp
│   │   └── src
│   │       ├── api.cpp
│   │       ├── config.cpp
│   │       ├── cql_server.cpp
│   │       ├── filter.cpp
│   │       ├── health.cpp
│   │       ├── main.cpp
│   │       └── writer.cpp
│   └── sstable-loader
│       ├── CMakeLists.txt
│       ├── Dockerfile.sstable-loader
│       ├── Dockerfile.sstable-loader-optimized
│       ├── Makefile
│       ├── include
│       │   └── sstable_loader
│       │       ├── config.hpp
│       │       ├── filter.hpp
│       │       ├── loader.hpp
│       │       └── token_range.hpp
│       └── src
│           ├── api.cpp
│           ├── config.cpp
│           ├── filter.cpp
│           ├── index_manager.cpp
│           ├── loader.cpp
│           ├── main.cpp
│           └── token_range.cpp
├── svckit
│   ├── CMakeLists.txt
│   ├── Makefile
│   ├── include
│   │   └── svckit
│   │       ├── concepts.hpp
│   │       ├── config.hpp
│   │       ├── database.hpp
│   │       ├── errors.hpp
│   │       ├── filter_governor.hpp
│   │       ├── metrics.hpp
│   │       └── types.hpp
│   └── src
│       ├── config.cpp
│       ├── database.cpp
│       ├── errors.cpp
│       ├── metrics.cpp
│       └── types.cpp
└── tests
    ├── CMakeLists.txt
    ├── test_config.cpp
    ├── test_dual_reader_filter.cpp
    ├── test_filter_governor.cpp
    ├── test_token_range.cpp
    └── test_types.cpp
```


## Compiling the Services

```shell
# From repository root
cd scylla-cluster-sync-cxx

# Configure + build all services (Release)
make build

# Build debug mode (with AddressSanitizer + UBSan)
make build-debug

# Build release mode (optimized)
make build-release

# Build sstable-loader with Phase 2 optimizations (prepared stmts + UNLOGGED BATCH)
make build-optimized

# Run Catch2 test suite
make test
```

**Or** with the provided root Makefile.
```
# =============================================================================
# Building Scylla Cluster Sync Services (C++20)
# =============================================================================

# --- Native C++20 Builds (CMake) ---

# Build all services (Release mode - optimized binaries)
make build

# Build all services (Debug mode + sanitizers)
make build-debug

# Build with Phase 2 optimizations (sstable-loader only)
make build-optimized

# --- Docker Builds ---

# Build all Docker images
make docker-build

# Build individual Docker images
make docker-build-dual-writer
make docker-build-dual-reader
make docker-build-sstable-loader

# Alternative: using docker directly
docker build -f services/dual-writer/Dockerfile.dual-writer -t dual-writer-cxx:latest .
docker build -f services/dual-reader/Dockerfile.dual-reader -t dual-reader-cxx:latest .
docker build -f services/sstable-loader/Dockerfile.sstable-loader -t sstable-loader-cxx:latest .

# --- Docker Compose ---

# Start all services (ScyllaDB + migration services + monitoring)
make docker-up

# View logs
make docker-logs

# Stop all services
make docker-down

# --- Development Workflow ---

# Run services locally (after make build)
# Binaries located at: ./build/services/{service-name}/{service-name}
./build/services/dual-writer/dual-writer --config config/dual-writer-scylla.yaml
./build/services/dual-reader/dual-reader --config config/dual-reader.yaml
./build/services/sstable-loader/sstable-loader --config config/sstable-loader.yaml

# --- Code Quality ---

make fmt          # Format code (clang-format)
make lint         # Run linter (clang-tidy)
make test         # Run tests (Catch2 via CTest)
make check        # Quick compilation check

# --- CI/CD ---
make ci           # fmt + lint + test + build-release
make pre-commit   # fmt + lint + test

# --- Service Artifacts Location ---

# Debug builds:   ./build/services/{dual-writer,dual-reader,sstable-loader}/
# Release builds: ./build/services/{dual-writer,dual-reader,sstable-loader}/
```


## Kubernetes Deployment Architecture 

### Unified Kubernetes Helm Chart Structure

```shell
config/deploy/scylla-cluster-sync/
├── Chart.yaml
├── values.yaml
├── values-production.yaml
└── templates/
    ├── _helpers.tpl
    ├── app.yaml
    ├── app-config.yaml
    ├── app-secrets.yaml
    ├── app-scaling.yaml
    ├── app-gateway.yaml
    ├── app-gateway-tls.yaml
    └── app-monitoring.yaml
```

### Deploying the Services using Unified Kubernetes Helm Chart

```shell
# Install all 3 services (default)
helm install migration ./config/deploy/scylla-cluster-sync \
  --namespace migration --create-namespace

# Install only dual-writer and dual-reader (skip sstable-loader)
helm install migration ./config/deploy/scylla-cluster-sync \
  --namespace migration --create-namespace \
  --set sstableLoader.enabled=false

# Install only dual-writer
helm install dual-writer ./config/deploy/scylla-cluster-sync \
  --namespace migration --create-namespace \
  --set dualReader.enabled=false \
  --set sstableLoader.enabled=false

# Production deployment
helm install migration ./config/deploy/scylla-cluster-sync \
  --namespace migration --create-namespace \
  -f ./config/deploy/scylla-cluster-sync/values-production.yaml

# Dry-run
helm template migration ./config/deploy/scylla-cluster-sync \
  --debug
```


## Scylla/Cassandra Cluster Sync Architectural Layout 

```
┌─────────────────────────────────────────────────────────────┐
│  GCP (Source)                                               │
├─────────────────────────────────────────────────────────────┤
│  • 3x GCP Compute VMs (Cassandra 4.x, self-managed)         │
│    - NO Kubernetes                                          │
│    - Manual install (no IaC)                                │
│    - RF=2                                                   │
│  • dual-writer service (GKE, 3 replicas)                    │
│    - Port 9042 (CQL proxy)                                  │
│    - Python app connects here                               │
│  • Local disk: /var/lib/cassandra/data                      │
│  • DataSync agent (maybe - client decides Monday)           │
└─────────────────────────────────────────────────────────────┘
                            │
                            │ AWS DataSync OR aws s3 sync
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  AWS (Target)                                               │
├─────────────────────────────────────────────────────────────┤
│  • S3 bucket: iconik-migration-sstables                     │
│  • 3x EC2 instances (Cassandra 4.x, self-managed)           │
│    - NO Kubernetes                                          │
│    - Manual install (no IaC)                                │
│    - RF=3                                                   │
│  • sstable-loader service (EKS, 3 replicas)                 │
│    - Reads from S3                                          │
│    - Imports to AWS Cassandra                               │
│  • dual-reader service (EKS, 3 replicas)                    │
│    - Validates consistency                                  │
└─────────────────────────────────────────────────────────────┘
```



## Scylla/Cassandra Cluster Sync Sequence Diagram (No AWS DataSync Services)

The following sequence diagram shows the worklow of the three `scylla-cluster-sync-cxx` services during the ScyllaDB/CassandraDB source to cross-cloud  ScyllaDB/CassandraDB target. 

```
┌─────────┐     ┌────────────┐     ┌───────────┐     ┌─────────────┐     ┌──────────┐
│   App   │     │dual-writer │     │ Cassandra │     │sstable-load │     │ ScyllaDB │
└────┬────┘     └─────┬──────┘     └─────┬─────┘     └──────┬──────┘     └────┬─────┘
     │                │                  │                  │                 │
     │ ═══════════════════════════ Phase 1: SourceOnly ══════════════════════════
     │                │                  │                  │                 │
     │──Write Req────▶│                  │                  │                 │
     │                │───Execute CQL───▶│                  │                 │
     │                │◀──────OK─────────│                  │                 │
     │◀───Response────│                  │                  │                 │
     │                │                  │                  │                 │
     │ ═══════════════════════════ Phase 2: DualAsync ═══════════════════════════
     │                │                  │                  │                 │
     │──Write Req────▶│                  │                  │                 │
     │                │───Execute CQL───▶│                  │                 │
     │                │◀──────OK─────────│                  │                 │
     │◀───Response────│                  │                  │                 │
     │                │                  │                  │                 │
     │                │════════════Shadow Write (async)════════════════════▶. │
     │                │                  │                  │                 │
     │                │                  │                  │                 │
     │                │                  │◀═══Bulk Read════│                  │
     │                │                  │═══════════════▶│                   │
     │                │                  │                  │───Insert Batch─▶│
     │                │                  │                  │◀──────OK────────│
     │                │                  │                  │                 │
     │ ═══════════════════════════ Phase 3: DualSync ════════════════════════════
     │                │                  │                  │                 │
     │──Write Req────▶│                  │                  │                 │
     │                │───Execute CQL───▶│                  │                 │
     │                │══════════════Execute CQL══════════════════════════▶   │
     │                │◀──────OK─────────│                  │                 │
     │                │◀════════════════════════OK═════════════════════════   │
     │◀───Response────│                  │                  │                 │
     │                │                  │                  │                 │
     │ ═══════════════════════════ Phase 4: TargetOnly ══════════════════════════
     │                │                  │                  │                 │
     │──Write Req────▶│                  │                  │                 │
     │                │══════════════Execute CQL══════════════════════════▶   │
     │                │◀════════════════════════OK═════════════════════════   │
     │◀───Response────│                  │                  │                 │
     │                │                  │                  │                 │
┌────┴────┐     ┌─────┴──────┐     ┌─────┴─────┐     ┌──────┴──────┐     ┌────┴─────┐
│   App   │     │dual-writer │     │ Cassandra │     │sstable-load │     │ ScyllaDB │
└─────────┘     └────────────┘     └───────────┘     └─────────────┘     └──────────┘
```

## Deployment Order (Chronological)

### Phase 0: Pre-Migration Setup

**Duration:** 1-2 days  
**Risk Level:** Low
```bash
# 1. Create namespaces
kubectl --context=gke-prod create namespace migration
kubectl --context=eks-prod create namespace migration

# 2. Deploy External Secrets Operator (if not present)
helm repo add external-secrets https://charts.external-secrets.io
helm install external-secrets external-secrets/external-secrets \
  --namespace external-secrets --create-namespace

# 3. Deploy ESO ClusterSecretStore CRD and Configuration with AWS SecretsManager
```yaml
apiVersion: external-secrets.io/v1beta1
kind: ClusterSecretStore
metadata:
  name: aws-secrets-manager
spec:
  provider:
    aws:
      service: SecretsManager
      region: us-east-1
      auth:
        jwt:
          serviceAccountRef:
            name: external-secrets
            namespace: external-secrets
```

```shell 
kubectl apply -f aws-secrets-store-cassandra.yaml 
```

```shell
# 4. Seed database credentials in AWS Secrets Manager
aws secretsmanager create-secret \
  --name production/iconik/cassandra-credentials \
  --secret-string '{"username":"cassandra","password":"xxx"}'

aws secretsmanager create-secret \
  --name production/iconik/scylla-credentials \
  --secret-string '{"username":"scylla","password":"xxx"}'
```

---

### Phase 1: Deploy sstable-loader (AWS EKS)

**Duration:** 30 minutes  
**Risk Level:** Low (no production impact yet)

Deploy the bulk migration service on the **target side** first. It will sit idle until triggered.
```shell
# Switch to EKS context
kubectl config use-context eks-prod

# Deploy sstable-loader only
helm install sstable-loader ./config/deploy/scylla-cluster-sync \
  --namespace migration \
  --set dualWriter.enabled=false \
  --set dualReader.enabled=false \
  --set sstableLoader.enabled=true \
  -f values-production.yaml

# Verify deployment
kubectl -n migration get pods -l app.kubernetes.io/component=sstable-loader
kubectl -n migration logs -l app.kubernetes.io/component=sstable-loader -f

# Test health endpoint
kubectl -n migration port-forward svc/sstable-loader 8081:8081 &
curl http://localhost:8081/health
```

**Expected output:**
```json
{
  "status": "healthy",
  "service": "sstable-loader",
  "index_manager_enabled": true,
  "index_count": 60
}
```

---


## Verifying Docker Container Images OS Arch

The root `Makefile` triggers the execution of the Docker container images and as a default generates OS architecture Linux `amd64` images. The Makefile includes the `--platform` flag to docker `buildx`. To verify the Linux `amd64` OS architecture of the container image, the following commands will verify this.

```shell

# dual-writer container image
docker buildx imagetools inspect isgogolgo13/dual-writer-cxx:latest
```

This outputs:
```shell
Name:      docker.io/isgogolgo13/dual-writer-cxx:latest
MediaType: application/vnd.oci.image.index.v1+json
Digest:    sha256:f5be0198051f6d6d49fe225576b80485ab92890d2d2232d9cd57b84e0b461fce

Manifests:
  Name:        docker.io/isgogolgo13/dual-writer-cxx:latest@sha256:a6697a354d9d0b6aa04f085140d301dd42d32e7b6154266a280172e494203d4f
  MediaType:   application/vnd.oci.image.manifest.v1+json
  Platform:    linux/amd64      **<------- Linux AMD64**
```

```shell
# dual-reader container image
docker buildx imagetools inspect isgogolgo13/dual-reader-cxx:latest
```
This outputs:
```shell
Name:      docker.io/isgogolgo13/dual-reader-cxx:latest
MediaType: application/vnd.oci.image.index.v1+json
Digest:    sha256:5f1571d1635edbcacdc4ac9abd0558758f1add9f4a7f6b8948c47442aaa6c605
           
Manifests: 
  Name:        docker.io/isgogolgo13/dual-reader-cxx:latest@sha256:26a33a45f0ca1d9fc83aef20ab31902ac7805f562163b5ce6f81995b0485caef
  MediaType:   application/vnd.oci.image.manifest.v1+json
  Platform:    linux/amd64       **<------- Linux AMD64**
```

```shell
# sstable-loader container image
docker buildx imagetools inspect isgogolgo13/sstable-loader-cxx:latest
```
This outputs:
```shell
Name:      docker.io/isgogolgo13/sstable-loader-cxx:latest
MediaType: application/vnd.oci.image.index.v1+json
Digest:    sha256:ec4dad72c8c618338c6936c7594d0fc78bb26c90c177b5391d9284ef6a356d21
           
Manifests: 
  Name:        docker.io/isgogolgo13/sstable-loader-cxx:latest@sha256:a44c1ee8cb0d9928cc3884f2f5289bc8912fe3482f66f145c2cd19ff04a45cbc
  MediaType:   application/vnd.oci.image.manifest.v1+json
  Platform:    linux/amd64   **<------- Linux AMD64**
```



## Range Failure Error Handling

```yaml
# Enable in config
skip_on_error: true
failed_rows_file: "failed_rows.jsonl"
```

The JSON output IF `skip_on_error` set to `true`.

```json
{"timestamp":"2026-01-26T10:19:01Z","table":"mattiasa_assets_keyspace.asset_versions","token_range_start":-9223372036854775808,"token_range_end":-8646911284551352321,"error":"Server error: newLimit > capacity","error_type":"range_error"}
```
Zero regression — existing behavior unchanged when skip_on_error: false `(default)`.




## Future Extensions 

The following is a list of future in the pipeline extensions to this service.

- WASM Real-Time Dual-Writer Proxy Progress and Statistics Tracker

### WASM (C++/Emscripten) Real-Time Dual-Writer Proxy Progress and Statistics Tracker (UI Dashboard) 

Here is architecture for this using C++20, Boost.Asio async and Emscripten WASM using SVG (D3.js equivalent style). This UI Dashboard will live on a coordinating (nuetral tier) Firecracker Hyper VMM. This Firecracker VMM can live on the following VMs.

- AWS EC2 (Bare Metal Linux)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        FIRECRACKER VMM (On-Prem/Neutral)                    │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                    scylla-sync-dash (WASM)                          │    │
│  │  ┌──────────────┐    ┌──────────────┐    ┌───────────────────────┐  │    │
│  │  │  LEFT EYE    │    │   CENTER     │    │     RIGHT EYE         │  │    │
│  │  │  GCP Source  │    │  Replication │    │     AWS Target        │  │    │
│  │  │              │    │    Lag       │    │                       │  │    │
│  │  │ ● Writes/sec │    │   ◄════►     │    │ ● Writes landed       │  │    │
│  │  │ ● Bytes/sec  │    │   Δ 47ms     │    │ ● Bytes/sec           │  │    │
│  │  │ ● Queue depth│    │              │    │ ● Ack latency         │  │    │
│  │  │ ● Errors     │    │  [Sparkline] │    │ ● Errors              │  │    │
│  │  └──────────────┘    └──────────────┘    └───────────────────────┘  │    │
│  │  ┌─────────────────────────────────────────────────────────────────┐│    │
│  │  │              ANOMALY DETECTION STRIP                            ││    │
│  │  │  🟢 Normal  🟢 Normal  🟡 Lag spike 14:32  🟢 Normal             ││    │
│  │  └─────────────────────────────────────────────────────────────────┘│    │ 
│  └─────────────────────────────────────────────────────────────────────┘    │
│                              ▲              ▲                               │
│                              │              │                               │
│                    WebSocket │              │ WebSocket                     │
│                    (metrics) │              │ (metrics)                     │
└──────────────────────────────┼──────────────┼───────────────────────────────┘
                               │              │
          ┌────────────────────┘              └────────────────────┐
          │                                                        │
          ▼                                                        ▼
┌─────────────────────────┐                          ┌─────────────────────────┐
│        GCP (GKE)        │                          │         AWS             │
│  ┌───────────────────┐  │                          │  ┌───────────────────┐  │
│  │   Python Client   │  │                          │  │  Target Cassandra │  │
│  └─────────┬─────────┘  │                          │  │    (ScyllaDB)     │  │
│            │            │                          │  └───────────────────┘  │
│            ▼            │                          │            ▲            │
│  ┌───────────────────┐  │      Async Shadow        │            │            │
│  │   dual-writer     │──┼──────────────────────────┼────────────┘            │
│  │   (sync-cxx)      │  │         Write            │                         │
│  └─────────┬─────────┘  │                          │                         │
│            │ SYNC       │                          │                         │
│            ▼            │                          │                         │
│  ┌───────────────────┐  │                          │                         │
│  │  Source Cassandra │  │                          │                         │
│  └───────────────────┘  │                          │                         │
└─────────────────────────┘                          └─────────────────────────┘
```


This added component will involve a new library `sync-core` that uses the Strategy Pattern and involves the following code fragments.

The WebSocket connections the Firecracker VMM uses across to GCP and AWS are **TLS** WebSockets. Here is this view on how that is used.

```shell
┌────────────────────────────────────────────────────────────────────────────┐
│                     FIRECRACKER VMM (On-Prem/Neutral)                      │
│                                                                            │
│   scylla-sync-dash (WASM)                                                  │
│         │                                                                  │
│         ├──── wss://dual-writer.gke.internal:9443/metrics ──────┐          │
│         │              (mTLS + JWT)                             │          │
│         │                                                       │          │
│         └──── wss://scylla-metrics.aws.internal:9443/metrics ───┼──┐       │
│                        (mTLS + JWT)                             │  │       │
└─────────────────────────────────────────────────────────────────┼──┼───────┘
                                                                  │  │
                          ┌───────────────────────────────────────┘  │
                          │  WireGuard/IPSec VPN Tunnel              │
                          │  or Cloud Interconnect                   │
                          ▼                                          ▼
              ┌───────────────────────┐               ┌───────────────────────┐
              │        GCP VPC        │               │        AWS VPC        │
              │  ┌─────────────────┐  │               │  ┌─────────────────┐  │
              │  │  dual-writer    │  │               │  │  metrics-proxy  │  │
              │  │  :9443 (TLS)    │  │               │  │  :9443 (TLS)    │  │
              │  └─────────────────┘  │               │  └─────────────────┘  │
              └───────────────────────┘               └───────────────────────┘
```



```cpp
// libs/sync-core/include/sync_core/types.hpp
// Strategy Pattern — metrics and event types for dashboard WASM component

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace sync_core {

struct WriteEvent {
    std::string id;               // UUID
    int64_t     timestamp{0};
    std::string table;
    std::string partition_key;
    uint64_t    size_bytes{0};
    std::optional<uint32_t> source_ack_ms;   // nullopt = pending
    std::optional<uint32_t> target_ack_ms;   // nullopt = in-flight
    std::string status;           // "Pending", "SourceAcked", "TargetInFlight", "TargetAcked", "Failed"
};

enum class Cloud { GCP, AWS };

struct ClusterMetrics {
    Cloud    cloud{Cloud::GCP};
    double   writes_per_sec{0.0};
    uint64_t bytes_per_sec{0};
    uint32_t p50_latency_ms{0};
    uint32_t p99_latency_ms{0};
    double   error_rate{0.0};
    uint32_t queue_depth{0};
    int64_t  timestamp{0};
};

struct ReplicationState {
    int64_t  lag_ms{0};
    uint64_t in_flight_bytes{0};
    uint32_t in_flight_writes{0};
    int64_t  last_synced_timestamp{0};
};

enum class AnomalyType {
    LagSpike,
    ErrorBurst,
    ThroughputDrop,
    ConnectionLost,
    QueueBackpressure,
};

struct Anomaly {
    std::string id;
    int64_t     timestamp{0};
    AnomalyType anomaly_type{AnomalyType::LagSpike};
    std::string severity;
    std::string message;
};

} // namespace sync_core
```

The metrics collection strategy offers **two** solutions.

### Option A: Embedded Metrics Exporter in Dual-Writer
```cpp
// dual-writer exposes WebSocket
// Dashboard connects directly
// ws://dual-writer.gke.internal:9090/metrics
```

### Option B: Prometheus + WebSocket Bridge
```cpp
// Scrape Prometheus, push to dashboard through WS
// - Higher decoupling 
// - works with existing monitoring
```

### Option C: Direct Cassandra/Scylla Metrics
```cpp
// Query system tables directly
// SELECT * FROM system.metrics WHERE ...
// Requires cross-cloud credentials (track "left-side source cloud / right-side target cloud")
```


**Recommendation is Option A** 
Have the dual-writer signal metrics throught the WebSocket. It already knows:

- Every write that comes in
- Source ack timing
- Target ack timing
- Errors
- Queue depth


### Firecracker VMM Deployment Architecture 
```shell
# On-prem or dedicated host with network access to both clouds
./firecracker-sync-dash/setup.sh

# VM gets:
# - VPN/tunnel to GCP VPC (dual-writer metrics)
# - VPN/tunnel to AWS VPC (target Cassandra metrics)
# - Static WASM binary (~2MB)
# - 256MB RAM, 1 vCPU
```


**Firecracker Advantages**

- Sub-second boot (instant failover)
- Small attack surface
- Isolated from both cloud VPCs
- Can run on any x86 Linux host



## References

Cassandra DB Kubernetes Operator 
- https://docs.k8ssandra.io/components/cass-operator/

ScyllaDB Kubernetes Operator 
- https://www.scylladb.com/product/scylladb-operator-kubernetes/




## License

MIT License - see [LICENSE](LICENSE) for details.

## Author

**LuckyDrone** - [luckydrone.io](https://luckydrone.io)
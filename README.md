# ScyllaDB Cluster-to-Cluster Sync Service (C++20)
Ultra-Fast RPO/RTO Sensitive ScyllaDB (Cassandra DB Adaptive) Cluster to Cluster Tenant Data Synching Service in C++20 using Shadow Write Transition Pattern.

The architecture of the ScyllaDB (or Cassandra DB source) to ScyllaDB (or Cassandra DB sink/target) involves three services to fulfill the tenant data lift-and-shift transition. These services are.

- **Dual-Writer Proxy Service** (C++20/23+)
- **SSTableLoader Processor Service** (C++20/23+)
- **Dual-Reader Data Sync Ack Service** (C++20/23+)

Rust or C++ is required to avoid any GC pauses that would fail 99999 SLA delivery as the data shuttling across the source to the target can NOT withstand latency delays.
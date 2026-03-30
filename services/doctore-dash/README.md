# 🏛️ Doctore Dashboard

> *"Train your data for the migration arena"*

Real-time database migration control panel for the scylla-cluster-sync-cxx suite.

**Polyglot frontend**: Leptos/WASM compiled to a static SPA served by nginx, connecting to the C++20 sstable-loader backend via REST API. Zero coupling to the backend language — the contract is the JSON `/status` endpoint.

## Quick Start

### Prerequisites

```bash
# Install Rust WASM target
rustup target add wasm32-unknown-unknown

# Install Trunk (WASM bundler)
cargo install trunk
```

### Development (Mock Mode)

```bash
cd services/doctore-dash

# Run development server with hot reload
trunk serve

# Opens http://localhost:3000 automatically
```

### Development (Live Mode — against C++ sstable-loader)

```bash
# Terminal 1: Start C++ sstable-loader
make build && ./build/services/sstable-loader/sstable-loader --config config/sstable-loader.yaml

# Terminal 2: Start doctore-dash with API proxy
cd services/doctore-dash && trunk serve --proxy-backend=http://localhost:8081
```

### Docker (from project root)

```bash
# Build and run with docker-compose (includes C++ services + doctore-dash)
make docker-up

# Access dashboard at http://localhost:3000
```

### Standalone Docker

```bash
cd services/doctore-dash
docker-compose up --build

# Access at http://localhost:3000 (mock mode)
```

## Features

- **Real-time Progress** — Live migration status with donut chart
- **Throughput Graph** — Rolling 60-second throughput visualization (Charming/SVG)
- **Table Progress** — Per-table migration status
- **Filter Stats** — Tenant/table filtering statistics
- **Event Log** — Live event stream
- **Controls** — Start/Pause/Stop/Config

## API Contract (C++ sstable-loader)

The dashboard connects to these endpoints (served by the C++20 sstable-loader):

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/status` | GET | Migration stats (total_rows, migrated_rows, throughput, etc.) |
| `/health` | GET | Service health check |
| `/metrics` | GET | Prometheus metrics |
| `/start` | POST | Start migration |
| `/stop` | POST | Stop migration |
| `/pause` | POST | Pause migration |
| `/resume` | POST | Resume migration |

## Color Palette

| Color | Hex | Use |
|-------|-----|-----|
| Crimson | `#DC143C` | Primary accent |
| Blood Red | `#8B0000` | Hover states |
| Gold | `#FFD700` | Success, highlights |
| Silver | `#C0C0C0` | Secondary elements |
| Charcoal | `#36454F` | Dark backgrounds |

## Architecture

```
doctore-dash/                    # Leptos/WASM (polyglot frontend)
├── src/
│   ├── main.rs                  # Entry point
│   ├── app.rs                   # Main app component
│   ├── state.rs                 # Reactive state (RwSignal)
│   ├── mock.rs                  # Mock data generator (demo mode)
│   └── components/
│       ├── header.rs            # Header with service status
│       ├── progress.rs          # Progress donut (Charming SVG)
│       ├── throughput.rs        # Throughput chart (Charming SVG)
│       ├── stats.rs             # Stats cards
│       ├── tables.rs            # Table progress list
│       ├── filter.rs            # Filter statistics
│       ├── controls.rs          # Control buttons
│       └── log.rs               # Event log
├── style/
│   └── main.css                 # Roman gladiator theme
├── nginx.conf                   # SPA serving + API proxy to C++ backend
├── Cargo.toml
├── Trunk.toml                   # WASM build config
└── Dockerfile                   # Multi-stage: trunk build → nginx
```

## Stack

- **Leptos** — Rust reactive UI framework (compiles to WASM)
- **Charming** — Rust charting library (Apache ECharts binding)
- **Trunk** — WASM build tool + dev server
- **nginx** — Production SPA serving + API reverse proxy
- **Backend** — C++20 sstable-loader (REST API)

---

**LuckyDrone.io** | High-performance migration technology

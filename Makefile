# =============================================================================
# Scylla Cluster Sync (C++20) - Makefile
# =============================================================================

DOCKER_REGISTRY ?= docker.io/isgogolgo13
PROJECT_NAME    := scylla-cluster-sync-cxx
VERSION         := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
CXX_STANDARD   := 20
BUILD_DIR       := build
BUILD_TYPE      ?= Release

# Target platform for GKE/EKS (linux/amd64)
PLATFORM        ?= linux/amd64
BUILDX_BUILDER  := multiarch

# =============================================================================
# Build Targets
# =============================================================================

.PHONY: build build-debug build-release clean configure

configure:
	@cmake -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_CXX_STANDARD=$(CXX_STANDARD) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	@cmake --build $(BUILD_DIR) --parallel $$(nproc)

build-debug:
	@$(MAKE) build BUILD_TYPE=Debug

build-release:
	@$(MAKE) build BUILD_TYPE=Release

## Phase 2 optimized builds for sstable-loader (prepared stmts + UNLOGGED BATCH + concurrent inserts)
## Original code path is preserved in default builds — these are additive only
build-optimized: configure
	@cmake -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=Release \
		-DSSTABLE_OPTIMIZED_INSERTS=ON
	@cmake --build $(BUILD_DIR) --parallel $$(nproc)

# =============================================================================
# Docker Buildx Setup
# =============================================================================

.PHONY: docker-setup

docker-setup:
	@echo "Setting up Docker buildx for cross-platform builds..."
	@docker buildx inspect $(BUILDX_BUILDER) >/dev/null 2>&1 || \
		docker buildx create --name $(BUILDX_BUILDER) --use --bootstrap
	@docker buildx use $(BUILDX_BUILDER)

# =============================================================================
# Docker Build (Cross-Platform)
# =============================================================================

.PHONY: docker-build docker-build-dual-writer docker-build-dual-reader docker-build-sstable-loader

docker-build: docker-setup docker-build-dual-writer docker-build-dual-reader docker-build-sstable-loader
	@echo "All images built for $(PLATFORM)"

docker-build-dual-writer: docker-setup
	@echo "Building dual-writer for $(PLATFORM)..."
	docker buildx build --platform $(PLATFORM) \
		-f services/dual-writer/Dockerfile.dual-writer \
		-t $(DOCKER_REGISTRY)/dual-writer-cxx:$(VERSION) \
		-t $(DOCKER_REGISTRY)/dual-writer-cxx:latest \
		--load .

docker-build-dual-reader: docker-setup
	@echo "Building dual-reader for $(PLATFORM)..."
	docker buildx build --platform $(PLATFORM) \
		-f services/dual-reader/Dockerfile.dual-reader \
		-t $(DOCKER_REGISTRY)/dual-reader-cxx:$(VERSION) \
		-t $(DOCKER_REGISTRY)/dual-reader-cxx:latest \
		--load .

docker-build-sstable-loader: docker-setup
	@echo "Building sstable-loader for $(PLATFORM)..."
	docker buildx build --platform $(PLATFORM) \
		-f services/sstable-loader/Dockerfile.sstable-loader \
		-t $(DOCKER_REGISTRY)/sstable-loader-cxx:$(VERSION) \
		-t $(DOCKER_REGISTRY)/sstable-loader-cxx:latest \
		--load .

# =============================================================================
# Docker Push
# =============================================================================

.PHONY: docker-push docker-push-dual-writer docker-push-dual-reader docker-push-sstable-loader

docker-push: docker-push-dual-writer docker-push-dual-reader docker-push-sstable-loader
	@echo "All images pushed to $(DOCKER_REGISTRY)"

docker-push-dual-writer:
	@echo "Pushing dual-writer..."
	docker push $(DOCKER_REGISTRY)/dual-writer-cxx:$(VERSION)
	docker push $(DOCKER_REGISTRY)/dual-writer-cxx:latest

docker-push-dual-reader:
	@echo "Pushing dual-reader..."
	docker push $(DOCKER_REGISTRY)/dual-reader-cxx:$(VERSION)
	docker push $(DOCKER_REGISTRY)/dual-reader-cxx:latest

docker-push-sstable-loader:
	@echo "Pushing sstable-loader..."
	docker push $(DOCKER_REGISTRY)/sstable-loader-cxx:$(VERSION)
	docker push $(DOCKER_REGISTRY)/sstable-loader-cxx:latest

# =============================================================================
# Docker Build + Push (Single Step - Faster)
# =============================================================================

.PHONY: docker-release docker-release-dual-writer docker-release-dual-reader docker-release-sstable-loader

docker-release: docker-setup docker-release-dual-writer docker-release-dual-reader docker-release-sstable-loader
	@echo "All images built and pushed to $(DOCKER_REGISTRY)"

docker-release-dual-writer: docker-setup
	@echo "Building and pushing dual-writer for $(PLATFORM)..."
	docker buildx build --platform $(PLATFORM) \
		-f services/dual-writer/Dockerfile.dual-writer \
		-t $(DOCKER_REGISTRY)/dual-writer-cxx:$(VERSION) \
		-t $(DOCKER_REGISTRY)/dual-writer-cxx:latest \
		--push .

docker-release-dual-reader: docker-setup
	@echo "Building and pushing dual-reader for $(PLATFORM)..."
	docker buildx build --platform $(PLATFORM) \
		-f services/dual-reader/Dockerfile.dual-reader \
		-t $(DOCKER_REGISTRY)/dual-reader-cxx:$(VERSION) \
		-t $(DOCKER_REGISTRY)/dual-reader-cxx:latest \
		--push .

docker-release-sstable-loader: docker-setup
	@echo "Building and pushing sstable-loader for $(PLATFORM)..."
	docker buildx build --platform $(PLATFORM) \
		-f services/sstable-loader/Dockerfile.sstable-loader \
		-t $(DOCKER_REGISTRY)/sstable-loader-cxx:$(VERSION) \
		-t $(DOCKER_REGISTRY)/sstable-loader-cxx:latest \
		--push .

# =============================================================================
# Phase 2 Optimized Docker Targets (sstable-loader only)
# =============================================================================

.PHONY: docker-build-sstable-loader-optimized docker-release-sstable-loader-optimized

docker-build-sstable-loader-optimized: docker-setup
	@echo "Building sstable-loader (Phase 2 optimized) for $(PLATFORM)..."
	docker buildx build --platform $(PLATFORM) \
		-f services/sstable-loader/Dockerfile.sstable-loader-optimized \
		-t $(DOCKER_REGISTRY)/sstable-loader-cxx:$(VERSION)-optimized \
		-t $(DOCKER_REGISTRY)/sstable-loader-cxx:optimized \
		--load .

docker-release-sstable-loader-optimized: docker-setup
	@echo "Building and pushing sstable-loader (Phase 2 optimized) for $(PLATFORM)..."
	docker buildx build --platform $(PLATFORM) \
		-f services/sstable-loader/Dockerfile.sstable-loader-optimized \
		-t $(DOCKER_REGISTRY)/sstable-loader-cxx:$(VERSION)-optimized \
		-t $(DOCKER_REGISTRY)/sstable-loader-cxx:optimized \
		--push .

# =============================================================================
# Docker Compose (Local Development)
# =============================================================================

.PHONY: docker-up docker-down docker-logs

docker-up:
	docker compose up -d

docker-down:
	docker compose down

docker-logs:
	docker compose logs -f

# =============================================================================
# Development
# =============================================================================

.PHONY: fmt lint test check clean

fmt:
	@find svckit services tests -name '*.cpp' -o -name '*.hpp' | \
		xargs clang-format -i --style=file
	@echo "Code formatted"

lint:
	@cmake --build $(BUILD_DIR) --target all 2>&1 | grep -E "warning:|error:" || \
		echo "No warnings"
	@if command -v clang-tidy >/dev/null 2>&1; then \
		find svckit services -name '*.cpp' | xargs clang-tidy \
			-p $(BUILD_DIR)/compile_commands.json; \
	fi

test: build
	@cd $(BUILD_DIR) && ctest --output-on-failure --parallel $$(nproc)

check: configure
	@cmake --build $(BUILD_DIR) --parallel $$(nproc) 2>&1

clean:
	@rm -rf $(BUILD_DIR)
	@docker buildx prune -f 2>/dev/null || true
	@echo "Clean complete"


# =============================================================================
# TUI Dashboard
# =============================================================================

.PHONY: tui-demo tui-dash tui-live

tui-demo: build
	@echo "Starting TUI Dashboard (Demo Mode)..."
	./$(BUILD_DIR)/services/tui-dash/tui-dash --demo

tui-dash: build
	@echo "Starting TUI Dashboard (Live Mode)..."
	./$(BUILD_DIR)/services/tui-dash/tui-dash --api-url http://localhost:9092

## make tui-live API_URL=http://sstable-loader.prod:9092
tui-live: build
	@echo "Starting TUI Dashboard (Live Mode)..."
	./$(BUILD_DIR)/services/tui-dash/tui-dash --api-url $(API_URL)



# =============================================================================
# Doctore Dashboard (Leptos WASM — polyglot frontend)
# =============================================================================

.PHONY: doctore-dev doctore-build doctore-docker

doctore-dev:
	@echo "Starting Doctore Dashboard (Dev Mode + Hot Reload)..."
	cd services/doctore-dash && trunk serve

doctore-dev-live:
	@echo "Starting Doctore Dashboard (Dev Mode → C++ sstable-loader backend)..."
	cd services/doctore-dash && trunk serve --proxy-backend=http://localhost:8081

doctore-build:
	@echo "Building Doctore Dashboard (WASM Release)..."
	cd services/doctore-dash && trunk build --release

doctore-docker: docker-setup
	@echo "Building Doctore Dashboard Docker image..."
	docker buildx build --platform $(PLATFORM) \
		-f services/doctore-dash/Dockerfile \
		-t $(DOCKER_REGISTRY)/doctore-dash:$(VERSION) \
		-t $(DOCKER_REGISTRY)/doctore-dash:latest \
		--load services/doctore-dash




# =============================================================================
# CI/CD
# =============================================================================

.PHONY: ci pre-commit

ci: fmt lint test build-release

pre-commit: fmt lint test

# =============================================================================
# Help
# =============================================================================

.PHONY: help

help:
	@echo ""
	@echo "Scylla Cluster Sync (C++20) - Makefile"
	@echo "======================================="
	@echo ""
	@echo "Docker (cross-platform for GKE/EKS):"
	@echo "  docker-build     - Build all images for $(PLATFORM) (local)"
	@echo "  docker-push      - Push all images to $(DOCKER_REGISTRY)"
	@echo "  docker-release   - Build AND push in one step (faster)"
	@echo ""
	@echo "Build:"
	@echo "  build            - Build all services (Release)"
	@echo "  build-debug      - Build all services (Debug + sanitizers)"
	@echo "  build-release    - Build all services (Release)"
	@echo "  build-optimized  - Build sstable-loader with Phase 2 optimizations"
	@echo ""
	@echo "Development:"
	@echo "  fmt              - Format code (clang-format)"
	@echo "  lint             - Run linter (clang-tidy)"
	@echo "  test             - Run tests (CTest + Catch2)"
	@echo "  clean            - Clean build artifacts"
	@echo ""
	@echo "Configuration:"
	@echo "  DOCKER_REGISTRY  - $(DOCKER_REGISTRY)"
	@echo "  PLATFORM         - $(PLATFORM)"
	@echo "  VERSION          - $(VERSION)"
	@echo "  BUILD_TYPE       - $(BUILD_TYPE)"
	@echo ""
	@echo "TUI Dashboard:"
	@echo "  tui-demo         - Run TUI dashboard in demo mode (no DB required)"
	@echo "  tui-dash         - Run TUI dashboard (connects to sstable-loader API)"
	@echo "  tui-live         - Run TUI dashboard with custom API_URL"

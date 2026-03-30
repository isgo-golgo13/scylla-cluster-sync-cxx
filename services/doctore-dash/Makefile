.PHONY: dev build build-release docker-build docker-run docker-up docker-down clean check

# Trunk development server (hot-reload)
dev:
	trunk serve

# Trunk development with live C++ backend proxy
dev-live:
	trunk serve --proxy-backend=http://localhost:8081

# Build WASM bundle (debug)
build:
	trunk build

# Build WASM bundle (release, optimized)
build-release:
	trunk build --release

# Docker build
docker-build:
	docker build -t doctore-dash:latest .

# Docker run (standalone, mock mode)
docker-run:
	docker run --rm -p 3000:3000 doctore-dash:latest

# Docker compose (standalone)
docker-up:
	docker-compose up -d

docker-down:
	docker-compose down

# Verify prerequisites
check:
	@echo "Checking prerequisites..."
	@rustup target list --installed | grep -q wasm32-unknown-unknown && \
		echo "  wasm32-unknown-unknown target: OK" || \
		echo "  wasm32-unknown-unknown target: MISSING (run: rustup target add wasm32-unknown-unknown)"
	@command -v trunk >/dev/null 2>&1 && \
		echo "  trunk: OK ($(trunk --version 2>/dev/null))" || \
		echo "  trunk: MISSING (run: cargo install trunk)"

clean:
	trunk clean
	rm -rf dist/

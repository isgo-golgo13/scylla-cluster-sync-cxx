.PHONY: build run test docker-build docker-run clean fmt lint check

SERVICE_NAME := dual-writer
DOCKER_IMAGE := ghcr.io/isgogolgo13/$(SERVICE_NAME)-cxx
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
BUILD_DIR := ../../build

CQL_PORT := 9042

build:
	@cd ../.. && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	@cd ../.. && cmake --build build --target $(SERVICE_NAME) --parallel $$(nproc)

run: build
	$(BUILD_DIR)/services/dual-writer/$(SERVICE_NAME) \
		--bind-addr 0.0.0.0:$(CQL_PORT) \
		--config ../../config/dual-writer-scylla.yaml \
		--filter-config ../../config/filter-rules.yaml

test:
	@cd ../.. && cmake --build build --target scylla_sync_tests --parallel $$(nproc)
	@cd $(BUILD_DIR) && ctest --output-on-failure -R filter_governor

docker-build:
	docker build -f Dockerfile.dual-writer -t $(DOCKER_IMAGE):$(VERSION) -t $(DOCKER_IMAGE):latest ../..

docker-run:
	docker run --rm -p $(CQL_PORT):$(CQL_PORT) \
		-v $(PWD)/../../config:/app/config:ro \
		$(DOCKER_IMAGE):latest

clean:
	@rm -rf $(BUILD_DIR)/services/dual-writer

fmt:
	@find . -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i --style=file

lint:
	@find . -name '*.cpp' | xargs clang-tidy -p $(BUILD_DIR)/compile_commands.json 2>/dev/null || true

check:
	@cd ../.. && cmake --build build --target $(SERVICE_NAME) 2>&1

build-all:
	@cd ../.. && cmake --build build --parallel $$(nproc)

test-all:
	@cd $(BUILD_DIR) && ctest --output-on-failure

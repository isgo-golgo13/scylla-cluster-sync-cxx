.PHONY: build build-optimized run test docker-build docker-run clean fmt lint

SERVICE_NAME := sstable-loader
DOCKER_IMAGE := ghcr.io/isgogolgo13/$(SERVICE_NAME)-cxx
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
BUILD_DIR := ../../build

build:
	@cd ../.. && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	@cd ../.. && cmake --build build --target $(SERVICE_NAME) --parallel $$(nproc)

build-optimized:
	@cd ../.. && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSSTABLE_OPTIMIZED_INSERTS=ON
	@cd ../.. && cmake --build build --target $(SERVICE_NAME) --parallel $$(nproc)

run: build
	$(BUILD_DIR)/services/sstable-loader/$(SERVICE_NAME) \
		--config ../../config/sstable-loader.yaml \
		--indexes ../../config/indexes.yaml \
		--filter-config ../../config/filter-rules.yaml \
		--port 8081

test:
	@cd ../.. && cmake --build build --target scylla_sync_tests --parallel $$(nproc)
	@cd $(BUILD_DIR) && ctest --output-on-failure -R token_range

docker-build:
	docker build -f Dockerfile.sstable-loader -t $(DOCKER_IMAGE):$(VERSION) -t $(DOCKER_IMAGE):latest ../..

docker-run:
	docker run --rm -p 8081:8081 \
		-v $(PWD)/../../config:/app/config:ro \
		$(DOCKER_IMAGE):latest

clean:
	@rm -rf $(BUILD_DIR)/services/sstable-loader

fmt:
	@find . -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i --style=file

lint:
	@find . -name '*.cpp' | xargs clang-tidy -p $(BUILD_DIR)/compile_commands.json 2>/dev/null || true

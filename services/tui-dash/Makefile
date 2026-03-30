.PHONY: build run-demo run-live test clean fmt lint

SERVICE_NAME := tui-dash
BUILD_DIR := ../../build

build:
	@cd ../.. && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	@cd ../.. && cmake --build build --target $(SERVICE_NAME) --parallel $$(nproc)

run-demo: build
	$(BUILD_DIR)/services/tui-dash/$(SERVICE_NAME) --demo

run-live: build
	$(BUILD_DIR)/services/tui-dash/$(SERVICE_NAME) --api-url http://localhost:9092

clean:
	@rm -rf $(BUILD_DIR)/services/tui-dash

fmt:
	@find . -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i --style=file

lint:
	@find . -name '*.cpp' | xargs clang-tidy -p $(BUILD_DIR)/compile_commands.json 2>/dev/null || true

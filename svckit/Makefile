.PHONY: build test clean fmt lint

BUILD_DIR := ../build

build:
	@cd .. && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	@cd .. && cmake --build build --target svckit --parallel $$(nproc)

test:
	@cd .. && cmake --build build --target scylla_sync_tests --parallel $$(nproc)
	@cd $(BUILD_DIR) && ctest --output-on-failure -R "config|types|filter_governor"

clean:
	@rm -rf $(BUILD_DIR)/svckit

fmt:
	@find . -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i --style=file

lint:
	@find . -name '*.cpp' | xargs clang-tidy -p $(BUILD_DIR)/compile_commands.json 2>/dev/null || true

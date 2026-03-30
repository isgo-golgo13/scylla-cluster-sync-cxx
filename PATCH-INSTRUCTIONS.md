# Patch Instructions for tui-dash Integration

## 1. Root CMakeLists.txt

Add the following line after the existing `add_subdirectory(services/dual-reader)` line:

```cmake
add_subdirectory(services/tui-dash)
```

The sub-directories section should now read:

```cmake
# ==============================================================================
# Sub-directories — order matters: svckit first (services depend on it)
# ==============================================================================
add_subdirectory(svckit)
add_subdirectory(services/dual-writer)
add_subdirectory(services/sstable-loader)
add_subdirectory(services/dual-reader)
add_subdirectory(services/tui-dash)
add_subdirectory(tests)
```

## 2. Root Makefile

Add the following section **before** the `# CI/CD` section:

```makefile
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
```

Also add `tui-demo` and `tui-dash` to the **help** target output:

```makefile
	@echo "TUI Dashboard:"
	@echo "  tui-demo         - Run TUI dashboard in demo mode (no DB required)"
	@echo "  tui-dash         - Run TUI dashboard (connects to sstable-loader API)"
	@echo "  tui-live         - Run TUI dashboard with custom API_URL"
	@echo ""
```

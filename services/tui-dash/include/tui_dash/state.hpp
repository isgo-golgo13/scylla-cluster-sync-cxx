#pragma once
// services/tui-dash/include/tui_dash/state.hpp
//
// Dashboard state management — aggregated migration stats, table list, activity log.
// C++20 port of services/tui-dash/src/state.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace tui_dash {

using Clock     = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<Clock>;

// =============================================================================
// TableStatus — per-table migration progress
// =============================================================================

struct TableStatus {
    std::string name;
    std::string status{"PENDING"};   // PENDING, RUNNING, DONE, FAILED, SKIPPED
    double      progress{0.0};       // 0.0 – 100.0
    uint64_t    rows_migrated{0};
    uint64_t    rows_total{0};
};

// =============================================================================
// LogEntry — single activity log line
// =============================================================================

struct LogEntry {
    TimePoint   timestamp{Clock::now()};
    std::string level;    // INFO, WARN, ERROR
    std::string message;
};

// =============================================================================
// DashboardState — full TUI state, updated by mock generator or API client
// =============================================================================

struct DashboardState {
    // Migration stats
    uint64_t total_rows{0};
    uint64_t migrated_rows{0};
    uint64_t failed_rows{0};
    uint64_t filtered_rows{0};
    double   throughput{0.0};
    double   progress_percent{0.0};

    // Table stats
    uint32_t tables_total{0};
    uint32_t tables_completed{0};
    uint32_t tables_skipped{0};

    // Range stats
    uint32_t ranges_total{0};
    uint32_t ranges_completed{0};

    // Status
    bool   is_running{false};
    bool   is_paused{false};
    double elapsed_secs{0.0};
    std::string elapsed_display{"00:00:00"};

    // Tables list
    std::vector<TableStatus> tables;

    // Activity log
    std::vector<LogEntry> activity_log;

    // UI state
    size_t scroll_offset{0};

    // --- Methods ---
    void toggle_pause();
    void reset();
    void scroll_up();
    void scroll_down();
    void add_log(std::string_view level, std::string_view message);
    void update_elapsed();
    void update_progress();
};

// Format large numbers: 1234567 → "1.23M"
[[nodiscard]] std::string format_number(uint64_t n);

// Format timestamp to HH:MM:SS string
[[nodiscard]] std::string format_time(const TimePoint& tp);

} // namespace tui_dash

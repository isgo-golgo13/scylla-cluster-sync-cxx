// services/tui-dash/src/state.cpp
//
// Dashboard state management implementation.
// C++20 port of services/tui-dash/src/state.rs
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "tui_dash/state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace tui_dash {

void DashboardState::toggle_pause() {
    if (is_running) {
        is_paused = !is_paused;
        add_log(is_paused ? "WARN" : "INFO",
                is_paused ? "Migration paused by user" : "Migration resumed");
    }
}

void DashboardState::reset() {
    *this = DashboardState{};
    add_log("INFO", "Dashboard reset");
}

void DashboardState::scroll_up() {
    if (scroll_offset > 0) --scroll_offset;
}

void DashboardState::scroll_down() {
    ++scroll_offset;
}

void DashboardState::add_log(std::string_view level, std::string_view message) {
    activity_log.push_back(LogEntry{
        Clock::now(),
        std::string{level},
        std::string{message},
    });

    // Keep last 100 entries
    if (activity_log.size() > 100) {
        activity_log.erase(activity_log.begin());
    }
}

void DashboardState::update_elapsed() {
    const auto total = static_cast<int>(elapsed_secs);
    const int hours   = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int seconds = total % 60;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, minutes, seconds);
    elapsed_display = buf;
}

void DashboardState::update_progress() {
    if (total_rows > 0) {
        progress_percent =
            (static_cast<double>(migrated_rows) / static_cast<double>(total_rows)) * 100.0;
    }
}

// =============================================================================
// Helpers
// =============================================================================

std::string format_number(uint64_t n) {
    char buf[32];
    if (n >= 1'000'000'000) {
        std::snprintf(buf, sizeof(buf), "%.2fB", static_cast<double>(n) / 1e9);
    } else if (n >= 1'000'000) {
        std::snprintf(buf, sizeof(buf), "%.2fM", static_cast<double>(n) / 1e6);
    } else if (n >= 1'000) {
        std::snprintf(buf, sizeof(buf), "%.2fK", static_cast<double>(n) / 1e3);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
    }
    return buf;
}

std::string format_time(const TimePoint& tp) {
    const auto tt = Clock::to_time_t(tp);
    std::tm local_tm{};
    localtime_r(&tt, &local_tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    return buf;
}

} // namespace tui_dash

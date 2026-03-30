// services/tui-dash/src/main.cpp
//
// TUI Dashboard for scylla-cluster-sync-cxx
// Terminal-based monitoring for SSTable migrations
//
// C++20 port of services/tui-dash/src/main.rs (ratatui → FTXUI)
//
// FTXUI is declarative/component-based:
//   - Renderer returns an Element tree each frame
//   - ScreenInteractive drives the event loop
//   - std::jthread runs the data update loop alongside FTXUI
//
// Run with:
//   ./tui-dash --demo                     # Demo mode (mock simulation)
//   ./tui-dash --api-url http://host:9092 # Live mode (sstable-loader)
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "tui_dash/state.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tui_dash {

// Forward declarations from mock.cpp and api.cpp
class MockDataGenerator;
class ApiClient;
std::unique_ptr<MockDataGenerator> create_mock_generator();
std::unique_ptr<ApiClient>         create_api_client(std::string_view url);

// The MockDataGenerator and ApiClient are opaque here; we call via their public update/fetch.
// We declare the update interface:
extern void mock_update(MockDataGenerator&, DashboardState&);
extern void api_fetch(ApiClient&, DashboardState&);

} // namespace tui_dash

// =============================================================================
// Since MockDataGenerator::update and ApiClient::fetch_status are member
// functions defined in their .cpp files, we bridge via wrapper functions.
// The classes are fully defined in their own TUs; main sees only the factory.
// We use a simple approach: the update loop calls the factories and holds
// the objects by unique_ptr, calling update/fetch via function pointers
// stored in the factories. For simplicity, we forward-declare and
// rely on the linker resolving the symbols from mock.o / api.o.
// =============================================================================

using namespace ftxui;

// =============================================================================
// Color palette — Red, White, Silver, Gold (matches Rust version exactly)
// =============================================================================

static const Color kRed       = Color::RGB(220, 50, 47);
static const Color kDarkRed   = Color::RGB(139, 0, 0);
static const Color kWhite     = Color::RGB(253, 246, 227);
static const Color kSilver    = Color::RGB(147, 161, 161);
static const Color kGold      = Color::RGB(255, 193, 37);
static const Color kDarkGold  = Color::RGB(184, 134, 11);
static const Color kBgDark    = Color::RGB(0, 20, 30);
static const Color kBgPanel   = Color::RGB(7, 30, 41);
static const Color kSuccess   = Color::RGB(133, 153, 0);
static const Color kError     = Color::RGB(220, 50, 47);

// =============================================================================
// Rendering helpers
// =============================================================================

static Element stat_box(const std::string& label, const std::string& value, Color value_color) {
    return vbox({
        text(label) | color(kSilver) | dim | center,
        text("") | center,
        text(value) | color(value_color) | bold | center,
    }) | border | flex | bgcolor(kBgPanel);
}

static Element render_header(const tui_dash::DashboardState& state, bool demo_mode, bool api_connected) {
    const auto status_text  = state.is_running
        ? (state.is_paused ? "[PAUSED]" : "[RUNNING]")
        : "[STOPPED]";
    const auto status_color = state.is_running
        ? (state.is_paused ? kGold : kSuccess)
        : kSilver;

    const auto mode_text  = demo_mode ? "DEMO" : (api_connected ? "LIVE" : "DISCONNECTED");
    const auto mode_color = demo_mode ? kGold  : (api_connected ? kSuccess : kRed);

    return hbox({
        text(" SCYLLA-CLUSTER-SYNC ") | color(kWhite) | bgcolor(kDarkRed) | bold,
        text("  "),
        text("TUI DASHBOARD") | color(kGold) | bold,
        text("  "),
        text(std::string{"["} + mode_text + "]") | color(mode_color) | bold,
        text("  "),
        text(status_text) | color(status_color) | bold,
        text("  "),
        text(state.elapsed_display) | color(kSilver),
    }) | center | borderEmpty;
}

static Element render_stats(const tui_dash::DashboardState& state) {
    return hbox({
        stat_box("TOTAL ROWS",  tui_dash::format_number(state.total_rows),    kWhite),
        stat_box("MIGRATED",    tui_dash::format_number(state.migrated_rows),  kSuccess),
        stat_box("FAILED",      tui_dash::format_number(state.failed_rows),
                 state.failed_rows > 0 ? kError : kSilver),
        stat_box("THROUGHPUT",
                 std::to_string(static_cast<int>(state.throughput)) + " rows/sec",
                 kGold),
    });
}

static Element render_progress(const tui_dash::DashboardState& state) {
    const float ratio = static_cast<float>(std::clamp(state.progress_percent, 0.0, 100.0) / 100.0);

    char pct_buf[16];
    std::snprintf(pct_buf, sizeof(pct_buf), "%.2f%%", state.progress_percent);

    return vbox({
        gauge(ratio) | color(kGold),
        hbox({
            text("Tables: ") | color(kSilver),
            text(std::to_string(state.tables_completed) + "/" + std::to_string(state.tables_total))
                | color(kWhite) | bold,
            text("  |  "),
            text("Skipped: ") | color(kSilver),
            text(std::to_string(state.tables_skipped))
                | color(state.tables_skipped > 0 ? kError : kSilver),
            text("  |  "),
            text("Ranges: ") | color(kSilver),
            text(std::to_string(state.ranges_completed) + "/" + std::to_string(state.ranges_total))
                | color(kWhite),
            text("  |  "),
            text(pct_buf) | color(kGold) | bold,
        }) | center,
    }) | border | color(kDarkGold) | bgcolor(kBgPanel);
}

static Element render_tables(const tui_dash::DashboardState& state) {
    Elements rows;

    // Header row
    rows.push_back(hbox({
        text("STATUS")   | color(kGold) | bold | size(WIDTH, EQUAL, 12),
        text("TABLE NAME") | color(kGold) | bold | flex,
        text("PROGRESS") | color(kGold) | bold | size(WIDTH, EQUAL, 10),
    }) | borderEmpty);

    for (const auto& t : state.tables) {
        Color status_color = kSilver;
        if (t.status == "DONE")    status_color = kSuccess;
        if (t.status == "RUNNING") status_color = kGold;
        if (t.status == "FAILED")  status_color = kError;

        char prog_buf[16];
        std::snprintf(prog_buf, sizeof(prog_buf), "%.1f%%", t.progress);

        rows.push_back(hbox({
            text(t.status) | color(status_color) | size(WIDTH, EQUAL, 12),
            text(t.name)   | color(kWhite)       | flex,
            text(prog_buf) | color(kSilver)      | size(WIDTH, EQUAL, 10),
        }));
    }

    return vbox(std::move(rows)) | border | bgcolor(kBgPanel) | flex;
}

static Element render_activity_log(const tui_dash::DashboardState& state) {
    Elements lines;

    // Show last 20 entries, newest first
    const size_t count = std::min(state.activity_log.size(), size_t{20});
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = state.activity_log[state.activity_log.size() - 1 - i];

        Color level_color = kSilver;
        std::string prefix = "[---]";
        if (entry.level == "INFO")  { prefix = "[INF]"; level_color = kSuccess; }
        if (entry.level == "WARN")  { prefix = "[WRN]"; level_color = kGold; }
        if (entry.level == "ERROR") { prefix = "[ERR]"; level_color = kError; }

        lines.push_back(hbox({
            text(tui_dash::format_time(entry.timestamp) + " ") | color(kSilver) | dim,
            text(prefix + " ") | color(level_color),
            text(entry.message) | color(kWhite),
        }));
    }

    return vbox(std::move(lines)) | border | bgcolor(kBgPanel) | flex;
}

static Element render_footer() {
    return hbox({
        text(" [Q] ") | color(kBgDark) | bgcolor(kRed),
        text(" Quit ") | color(kSilver),
        text("  "),
        text(" [SPACE] ") | color(kBgDark) | bgcolor(kGold),
        text(" Pause/Resume ") | color(kSilver),
        text("  "),
        text(" [R] ") | color(kBgDark) | bgcolor(kWhite),
        text(" Reset ") | color(kSilver),
        text("  "),
        text(" [UP/DOWN] ") | color(kBgDark) | bgcolor(kSilver),
        text(" Scroll ") | color(kSilver),
    }) | center | borderEmpty;
}

// =============================================================================
// Argument parsing
// =============================================================================

struct Args {
    bool        demo{false};
    std::string api_url{"http://localhost:9092"};
    int         refresh_ms{100};
};

static Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a{argv[i]};
        if (a == "--demo" || a == "-d") args.demo = true;
        else if ((a == "--api-url") && i + 1 < argc) args.api_url = argv[++i];
        else if ((a == "--refresh-ms") && i + 1 < argc) args.refresh_ms = std::stoi(argv[++i]);
    }
    return args;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    const auto args = parse_args(argc, argv);

    // Shared state protected by mutex
    tui_dash::DashboardState state;
    std::mutex               state_mutex;
    std::atomic<bool>        quit{false};
    bool                     api_connected{false};

    if (args.demo) {
        state.add_log("INFO", "TUI Dashboard started in DEMO mode");
    } else {
        state.add_log("INFO", "TUI Dashboard started - connecting to " + args.api_url);
    }

    // --- Data update loop in background jthread ---
    auto mock_gen  = args.demo ? tui_dash::create_mock_generator() : nullptr;
    auto api_client = !args.demo ? tui_dash::create_api_client(args.api_url) : nullptr;

    std::jthread update_thread([&](std::stop_token stoken) {
        while (!stoken.stop_requested() && !quit.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(args.refresh_ms));
            std::lock_guard lock{state_mutex};

            if (mock_gen) {
                mock_gen->update(state);
            }
            if (api_client) {
                api_client->fetch_status(state);
                api_connected = api_client->is_connected();
            }
        }
    });

    // --- FTXUI screen + renderer ---
    auto screen = ScreenInteractive::Fullscreen();

    auto renderer = Renderer([&] {
        std::lock_guard lock{state_mutex};

        return vbox({
            render_header(state, args.demo, api_connected),
            render_stats(state),
            render_progress(state),
            hbox({
                render_tables(state)      | size(WIDTH, EQUAL, 60),
                render_activity_log(state) | flex,
            }) | flex,
            render_footer(),
        }) | bgcolor(kBgDark);
    });

    // Key handler
    auto component = CatchEvent(renderer, [&](Event event) -> bool {
        std::lock_guard lock{state_mutex};

        if (event == Event::Character('q') || event == Event::Escape) {
            quit.store(true);
            screen.Exit();
            return true;
        }
        if (event == Event::Character(' ')) {
            state.toggle_pause();
            return true;
        }
        if (event == Event::Character('r')) {
            state.reset();
            return true;
        }
        if (event == Event::ArrowUp) {
            state.scroll_up();
            return true;
        }
        if (event == Event::ArrowDown) {
            state.scroll_down();
            return true;
        }
        return false;
    });

    // Run FTXUI event loop — blocks until screen.Exit()
    screen.Loop(component);

    // Signal update thread to stop
    quit.store(true);

    return 0;
}

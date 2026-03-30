// services/tui-dash/src/api.cpp
//
// API client — fetches live migration stats from sstable-loader /status endpoint.
// Uses cpp-httplib (synchronous GET with 500ms timeout).
//
// C++20 port of services/tui-dash/src/api.rs (reqwest → httplib)
//
// Copyright (c) 2025 LuckyDrone.io — All rights reserved.

#include "tui_dash/state.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace tui_dash {

using json = nlohmann::json;

// =============================================================================
// ApiClient — polls sstable-loader /status endpoint
// =============================================================================

class ApiClient {
public:
    explicit ApiClient(std::string_view base_url)
        : base_url_{base_url}
    {
        // Parse host:port from URL
        auto url = std::string{base_url};
        // Strip trailing slash
        while (!url.empty() && url.back() == '/') url.pop_back();

        // Parse "http://host:port"
        auto scheme_end = url.find("://");
        auto host_start = (scheme_end != std::string::npos) ? scheme_end + 3 : 0;
        auto port_sep   = url.find(':', host_start);

        if (port_sep != std::string::npos) {
            host_ = url.substr(host_start, port_sep - host_start);
            port_ = std::stoi(url.substr(port_sep + 1));
        } else {
            host_ = url.substr(host_start);
            port_ = 9092;
        }

        client_ = std::make_unique<httplib::Client>(host_, port_);
        client_->set_connection_timeout(0, 500'000);  // 500ms
        client_->set_read_timeout(0, 500'000);
    }

    void fetch_status(DashboardState& state) {
        auto res = client_->Get("/status");

        if (!res) {
            if (connected_) {
                state.add_log("WARN", "Lost connection to sstable-loader: " + httplib::to_string(res.error()));
            }
            connected_ = false;
            return;
        }

        if (res->status != 200) {
            connected_ = false;
            last_error_ = "HTTP " + std::to_string(res->status);
            return;
        }

        try {
            auto body = json::parse(res->body);
            connected_ = true;
            last_error_.clear();
            update_state(state, body);
        } catch (const json::exception& e) {
            connected_ = false;
            last_error_ = std::string{"Parse error: "} + e.what();
            state.add_log("ERROR", "Failed to parse API response: " + last_error_);
        }
    }

    [[nodiscard]] bool is_connected() const noexcept { return connected_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    void update_state(DashboardState& state, const json& body) {
        if (!body.contains("migration")) return;

        const auto& m = body["migration"];

        // Detect state transitions for logging
        const bool was_running = state.is_running;
        const bool was_paused  = state.is_paused;
        const auto prev_failed = state.failed_rows;

        // Update stats from JSON
        state.total_rows       = m.value("total_rows", uint64_t{0});
        state.migrated_rows    = m.value("migrated_rows", uint64_t{0});
        state.failed_rows      = m.value("failed_rows", uint64_t{0});
        state.filtered_rows    = m.value("filtered_rows", uint64_t{0});
        state.throughput       = m.value("throughput_rows_per_sec", 0.0);
        state.progress_percent = m.value("progress_percent", 0.0);
        state.tables_total     = m.value("tables_total", uint32_t{0});
        state.tables_completed = m.value("tables_completed", uint32_t{0});
        state.tables_skipped   = m.value("tables_skipped", uint32_t{0});
        state.is_running       = m.value("is_running", false);
        state.is_paused        = m.value("is_paused", false);
        state.elapsed_secs     = m.value("elapsed_secs", 0.0);
        state.update_elapsed();

        // Log state transitions
        if (!was_running && state.is_running) {
            state.add_log("INFO", "Connected to sstable-loader at " + base_url_);
        }
        if (was_running && !state.is_running && state.migrated_rows > 0) {
            state.add_log("INFO", "Migration completed");
        }
        if (!was_paused && state.is_paused) {
            state.add_log("WARN", "Migration paused");
        }
        if (was_paused && !state.is_paused && state.is_running) {
            state.add_log("INFO", "Migration resumed");
        }
        if (state.failed_rows > prev_failed && (state.failed_rows - prev_failed) > 100) {
            state.add_log("WARN",
                std::to_string(state.failed_rows - prev_failed) + " rows failed");
        }
    }

    std::string base_url_;
    std::string host_;
    int         port_{9092};
    std::unique_ptr<httplib::Client> client_;
    bool        connected_{false};
    std::string last_error_;
};

// Factory — used by main.cpp
std::unique_ptr<ApiClient> create_api_client(std::string_view url) {
    return std::make_unique<ApiClient>(url);
}

} // namespace tui_dash

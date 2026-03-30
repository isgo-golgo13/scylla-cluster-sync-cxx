# ==============================================================================
# tui-dash — FTXUI terminal dashboard for sstable-loader migration monitoring
# C++20 port of services/tui-dash (Rust ratatui → C++ FTXUI)
# ==============================================================================

add_executable(tui-dash)

target_sources(tui-dash
    PRIVATE
        src/main.cpp
        src/state.cpp
        src/mock.cpp
        src/api.cpp
)

target_include_directories(tui-dash
    PRIVATE
        include
        src
)

target_compile_features(tui-dash PRIVATE cxx_std_20)

target_link_libraries(tui-dash
    PRIVATE
        ftxui::screen
        ftxui::dom
        ftxui::component
        httplib::httplib
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

include(CompilerWarnings)
target_apply_warnings(tui-dash)

install(TARGETS tui-dash RUNTIME DESTINATION bin)

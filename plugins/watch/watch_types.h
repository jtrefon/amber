#ifndef AMBER_PLUGINS_WATCH_TYPES_H
#define AMBER_PLUGINS_WATCH_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace agent::plugins {

enum class WatchState : std::uint8_t {
    Queued,
    Running,
    Succeeded,
    Failed,
    TimedOut,
    Cancelled,
    Unknown,
};

const char* watch_state_name(WatchState state) noexcept;
WatchState watch_state_from(const std::string& name) noexcept;
bool watch_state_terminal(WatchState state) noexcept;

struct WatchReport {
    std::string watch_id;
    std::string report_id;
    WatchState state = WatchState::Unknown;
    std::string headline;
    std::vector<std::string> observed;
    std::string likely_cause;
    std::string next_action;
    std::string confidence = "low";
    bool needs_attention = false;
    std::string journal_update;
    std::string artifact_path;
    bool format_error = false;
    int attempt = 1;
};

struct WatchRecord {
    std::string id;
    std::string owner_id;
    std::string label;
    std::string profile; // process | pr | url
    std::string command;
    std::string pr;
    std::string url;
    std::string cwd;
    WatchState state = WatchState::Queued;
    long created_ms = 0;
    long started_ms = 0;
    long ended_ms = 0;
    int interval_s = 20;
    int deadline_s = 1800;
    int attempt = 1;
    int success_exit = 0;
    bool observer = true;
    // Delivery is host-owned. `delivered` is set by the host after it starts
    // the report turn; `abandoned` means the owner window is gone, so a restart
    // must not re-emit to it.
    bool delivered = false;
    bool abandoned = false;
    std::string head_sha;
    std::string previous_summary;
    std::string last_status;
    std::vector<std::string> journal;
    std::string report_id;
    std::string artifact_dir;
};

struct WatchStart {
    std::string owner_id;
    std::string profile = "process";
    std::string label;
    std::string command;
    std::string pr;
    std::string url;
    std::string cwd;
    int interval_s = 0; // 0 = watch.conf default
    int deadline_s = 0; // 0 = watch.conf default
    int success_exit = 0;
    bool observer = true;
};

struct WatchCounts {
    int running = 0;
    int attention = 0;
    int observers = 0;
};

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_TYPES_H

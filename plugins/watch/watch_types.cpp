#include "watch_types.h"

namespace agent::plugins {

const char* watch_state_name(WatchState state) noexcept {
    switch (state) {
    case WatchState::Queued:
        return "queued";
    case WatchState::Running:
        return "running";
    case WatchState::Succeeded:
        return "succeeded";
    case WatchState::Failed:
        return "failed";
    case WatchState::TimedOut:
        return "timed_out";
    case WatchState::Cancelled:
        return "cancelled";
    case WatchState::Unknown:
        break;
    }
    return "unknown";
}

WatchState watch_state_from(const std::string& name) noexcept {
    if (name == "queued")
        return WatchState::Queued;
    if (name == "running")
        return WatchState::Running;
    if (name == "succeeded")
        return WatchState::Succeeded;
    if (name == "failed")
        return WatchState::Failed;
    if (name == "timed_out")
        return WatchState::TimedOut;
    if (name == "cancelled")
        return WatchState::Cancelled;
    return WatchState::Unknown;
}

bool watch_state_terminal(WatchState state) noexcept {
    return state == WatchState::Succeeded || state == WatchState::Failed ||
           state == WatchState::TimedOut || state == WatchState::Cancelled ||
           state == WatchState::Unknown;
}

} // namespace agent::plugins

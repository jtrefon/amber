#include "log_retention_controller.h"

#include "agent/workspace.h"
#include "tui.h"
#include "window.h"
#include "window_manager.h"

#include <cstdint>
#include <ctime>
#include <string>

namespace tui {

namespace {

std::string mb_text(std::uint64_t bytes) {
    return std::to_string(bytes / (1024ull * 1024ull)) + " MB";
}

} // namespace

LogRetentionController::~LogRetentionController() {
    if (pass_.valid())
        pass_.wait();
}

std::vector<std::string> LogRetentionController::active_paths() const {
    std::vector<std::string> paths;
    for (const auto& window : tui_.window_manager_->all()) {
        if (!window->agent)
            continue;
        const std::string& path = window->agent->log().path();
        if (!path.empty())
            paths.push_back(path);
    }
    return paths;
}

void LogRetentionController::start(bool force) {
    // Consume a pass that finished but has not been reported yet: its future is
    // still "valid", and treating that as "in flight" would silently drop the
    // next pass -- which is how the manual trigger once did nothing.
    poll();
    if (pass_.valid())
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < next_)
        return;
    next_ = now + std::chrono::hours(1);
    std::string dir = agent::Workspace::state_dir() + "/logs";
    agent::RetentionPolicy policy = tui_.cfg_.log_retention_policy();
    std::vector<std::string> active = active_paths();
    const std::time_t wall = std::time(nullptr);
    pass_ = std::async(std::launch::async, [dir, policy, active, wall] {
        return agent::prune_logs(dir, policy, wall, active);
    });
}

void LogRetentionController::poll() {
    if (!pass_.valid())
        return;
    if (pass_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const agent::RetentionReport report = pass_.get();
    bool appended = false;
    if (!report.errors.empty()) {
        tui_.append_line(P_STATUS, "log retention: " + std::to_string(report.errors.size()) +
                                       " file(s) could not be removed: " + report.errors.front());
        appended = true;
    }
    if (report.active_over_cap) {
        tui_.append_line(P_STATUS, "log retention: the open transcript alone exceeds the cap, and "
                                   "is never deleted");
        appended = true;
    }
    if (report.removed > 0) {
        tui_.append_line(P_STATUS, "log retention: removed " + std::to_string(report.removed) +
                                       " log file(s), " + mb_text(report.before.bytes) + " -> " +
                                       mb_text(report.after.bytes));
        appended = true;
    }
    // This runs from the idle tick, which repaints only the clock while nothing
    // is busy: a new transcript line has to ask for the transcript itself, or it
    // sits in the window unpainted until the next keystroke.
    if (appended)
        tui_.render_engine_->draw();
}

} // namespace tui

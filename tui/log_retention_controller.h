#ifndef AMBER_TUI_LOG_RETENTION_CONTROLLER_H
#define AMBER_TUI_LOG_RETENTION_CONTROLLER_H

#include "agent/log_retention.h"

#include <chrono>
#include <future>
#include <vector>

namespace tui {

class Tui;

// Runs the retention policy off the UI thread and reports what it did: once at
// startup, at most hourly after that, and on demand from /set logs prune. The
// future is joined by the destructor, so a pass cannot outlive the Tui that
// started it. Kept out of Tui because the class is at its size cap and because a
// schedule plus a background pass is its own reason to change.
class LogRetentionController {
public:
    explicit LogRetentionController(Tui& tui) : tui_(tui) {}
    ~LogRetentionController();
    LogRetentionController(const LogRetentionController&) = delete;
    LogRetentionController& operator=(const LogRetentionController&) = delete;

    // Start a pass unless one is running. `force` ignores the hourly schedule.
    void start(bool force);
    // Consume a finished pass, if there is one, and report it. Once per tick.
    void poll();

private:
    std::vector<std::string> active_paths() const;

    Tui& tui_;
    std::future<agent::RetentionReport> pass_;
    std::chrono::steady_clock::time_point next_{};
};

} // namespace tui

#endif // AMBER_TUI_LOG_RETENTION_CONTROLLER_H

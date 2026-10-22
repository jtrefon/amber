#ifndef AMBER_PLUGINS_WATCH_PROFILES_H
#define AMBER_PLUGINS_WATCH_PROFILES_H

#include "watch_runner.h"
#include "watch_types.h"

#include "agent/process.h"

#include <functional>
#include <string>

namespace agent::plugins {

// What a profile needs from the service. Keeping these as callbacks makes the
// profile runnable against a fake runner/store in a unit test.
struct ProfileHooks {
    std::function<void(const WatchRecord&)> update;
    // Ask the observer for a refinement of the deterministic report. The
    // fallback is returned unchanged when observation is off/unavailable.
    std::function<WatchReport(const WatchRecord&, const WatchReport&, const CancellationToken&)>
        observe;
    // Store a full artifact and return its path (empty on failure).
    std::function<std::string(const std::string& name, const std::string& content)> artifact;
};

WatchReport run_process_profile(WatchRecord& record, CommandRunner& runner,
                                const ProfileHooks& hooks, const CancellationToken& cancel);

WatchReport run_pr_profile(WatchRecord& record, CommandRunner& runner, const ProfileHooks& hooks,
                           const CancellationToken& cancel);

WatchReport run_url_profile(WatchRecord& record, CommandRunner& runner, const ProfileHooks& hooks,
                            const CancellationToken& cancel);

// Shared helpers exposed for hermetic tests.
bool safe_pr_argument(const std::string& value);
bool safe_url(const std::string& value);
std::vector<std::string> failed_check_names(const std::string& checks_json);
bool checks_terminal(const std::string& checks_json);

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_PROFILES_H

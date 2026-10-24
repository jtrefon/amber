#ifndef AMBER_TUI_WORK_AUTO_ATTACH_H
#define AMBER_TUI_WORK_AUTO_ATTACH_H

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "agent/job.h"
#include "agent/subagent.h"

namespace tui {

// One change the tick should apply: open a background viewer for newly running
// work, or close the viewer of work that reached a terminal state.
struct AutoAttachStep {
    enum class Action { Open, Close, Forget };
    Action action = Action::Open;
    std::string kind; // "job" | "agent"
    std::string id;
    std::string title; // Open only
    std::string key;   // bookkeeping key ("job:<id>" / "agent:<id>")
};

// Pure decision table for `work.auto_attach`:
//   - newly running work opens one background viewer;
//   - terminal work drops its bookkeeping; it closes the viewer only when the
//     user never looked at it (`ever_focused`), otherwise it is left standing
//     with the final output.
// Focus is never part of a step, by construction.
std::vector<AutoAttachStep> plan_auto_attach(
    const std::string& mode, const std::vector<agent::JobInfo>& jobs,
    const std::vector<agent::SubAgentTaskInfo>& tasks,
    const std::unordered_set<std::string>& attached,
    const std::function<bool(const std::string& kind, const std::string& id)>& ever_focused);

} // namespace tui

#endif // AMBER_TUI_WORK_AUTO_ATTACH_H

#ifndef AMBER_TUI_WORKER_APPROVAL_H
#define AMBER_TUI_WORKER_APPROVAL_H

#include "agent/agent.h"
#include "tui/worker_target.h"

#include <string>

namespace tui {

class EventRouter;
class WindowManager;

// The hooks a sub-agent run owned by `owner_id` gets. Approval and API-key
// requests reach the owner window (or the active window when the owner is not
// focused) and block the worker on the same promise the foreground path uses.
// A closed owner, a shutting-down router, or no window at all fails closed:
// the worker never inherits an auto-approver just because it runs in the
// background.
agent::AgentHooks worker_hooks(EventRouter& router, WindowManager& windows,
                               const std::string& owner_id);

} // namespace tui

#endif // AMBER_TUI_WORKER_APPROVAL_H

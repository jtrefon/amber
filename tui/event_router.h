#ifndef AMBER_TUI_EVENT_ROUTER_H
#define AMBER_TUI_EVENT_ROUTER_H

#include "agent_event.h"
#include "window.h"

#include <memory>
#include <queue>
#include <vector>

namespace tui {

// Deliver an event to its origin window. A stamped event whose window no
// longer exists is dropped (never routed to a stranger window); unstamped
// events (npos) go to the active window.
inline Window* route_event(std::vector<std::unique_ptr<Window>>& windows,
                           const AgentEvent& ev, size_t active) {
    if (ev.window_id != std::string::npos) {
        if (ev.window_id >= windows.size()) return nullptr;
        return windows[ev.window_id].get();
    }
    if (active >= windows.size()) return nullptr;
    return windows[active].get();
}

// Shutdown path: deny-resolve every queued approval so a worker blocked on
// its promise can finish and be joined instead of deadlocking teardown.
inline void deny_all_pending_approvals(std::queue<AgentEvent>& q) {
    while (!q.empty()) {
        AgentEvent& ev = q.front();
        if (ev.approval_promise)
            ev.approval_promise->set_value(agent::Approval::Deny);
        q.pop();
    }
}

} // namespace tui

#endif // AMBER_TUI_EVENT_ROUTER_H

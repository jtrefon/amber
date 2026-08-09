#ifndef AMBER_TUI_EVENT_ROUTER_H
#define AMBER_TUI_EVENT_ROUTER_H

#include "agent_event.h"
#include "window.h"

#include <future>
#include <memory>
#include <queue>
#include <vector>

namespace tui {

// Deliver an event to its origin window. Stamped events match the window's
// stable id (Window::id), so erasing other windows never redirects them; an
// event whose window no longer exists is dropped, never routed to a stranger
// window. Unstamped events (npos) go to the active window.
inline Window* route_event(std::vector<std::unique_ptr<Window>>& windows,
                           const AgentEvent& ev, size_t active) {
    if (ev.window_id != std::string::npos) {
        for (auto& w : windows)
            if (w && w->id == ev.window_id) return w.get();
        return nullptr;
    }
    if (active >= windows.size()) return nullptr;
    return windows[active].get();
}

// Locate a window by its stable id; nullptr when it no longer exists.
inline Window* find_window(std::vector<std::unique_ptr<Window>>& windows,
                           size_t id) {
    for (auto& w : windows)
        if (w && w->id == id) return w.get();
    return nullptr;
}

// Shutdown path: deny-resolve every queued approval so a worker blocked on
// its promise can finish and be joined instead of deadlocking teardown. A
// promise that is already satisfied is skipped (set_value would throw
// std::future_error — and this runs from ~Tui, where an escaping exception
// would std::terminate).
inline void deny_all_pending_approvals(std::queue<AgentEvent>& q) {
    while (!q.empty()) {
        AgentEvent& ev = q.front();
        if (ev.approval_promise) {
            try {
                ev.approval_promise->set_value(agent::Approval::Deny);
            } catch (const std::future_error&) {
                // Already resolved by the UI thread; nothing to do.
            }
        }
        q.pop();
    }
}

} // namespace tui

#endif // AMBER_TUI_EVENT_ROUTER_H

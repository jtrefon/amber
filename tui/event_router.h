#ifndef AMBER_TUI_EVENT_ROUTER_H
#define AMBER_TUI_EVENT_ROUTER_H

#include "agent_event.h"
#include "window.h"

#include <agent.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace tui {
class Tui;

// Deliver an event to its origin window. Stamped events match the window's
// stable id (Window::id), so erasing other windows never redirects them; an
// event whose window no longer exists is dropped, never routed to a stranger
// window. Unstamped events (npos) go to the active window.
inline Window* route_event(std::vector<std::unique_ptr<Window>>& windows, const AgentEvent& ev,
                           size_t active) {
    if (ev.window_id != std::string::npos) {
        for (auto& w : windows)
            if (w && w->id == ev.window_id)
                return w.get();
        return nullptr;
    }
    if (active >= windows.size())
        return nullptr;
    return windows[active].get();
}

// Locate a window by its stable id; nullptr when it no longer exists.
inline Window* find_window(std::vector<std::unique_ptr<Window>>& windows, size_t id) {
    for (auto& w : windows)
        if (w && w->id == id)
            return w.get();
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

// Shutdown path for pending API-key requests: resolve with an empty key
// (user/UI gone — fail closed, no retry).
inline void deny_all_pending_api_keys(std::queue<AgentEvent>& q) {
    while (!q.empty()) {
        AgentEvent& ev = q.front();
        if (ev.api_key_promise) {
            try {
                ev.api_key_promise->set_value(std::string());
            } catch (const std::future_error&) {
                // Already resolved by the UI thread; nothing to do.
            }
        }
        q.pop();
    }
}

// Shutdown path for pending plugin questions: answer "cancelled"/"no" so a
// plugin blocked on its promise can finish instead of deadlocking teardown.
inline void deny_all_pending_asks(std::queue<AgentEvent>& q) {
    while (!q.empty()) {
        AgentEvent& ev = q.front();
        if (ev.ask_promise) {
            try {
                ev.ask_promise->set_value(AskAnswer{});
            } catch (const std::future_error&) {
                // Already resolved by the UI thread; nothing to do.
            }
        }
        q.pop();
    }
}

struct PendingToolLine {
    size_t index = std::string::npos;
    size_t window_id = std::string::npos;
    std::string name;
    std::string fingerprint;
    std::string tail;
    int frame = 0;
};

// Routes stamped AgentEvents from every window's worker to their origin
// window. Owns the shared event queue plus the modal-serialized question
// queues (approvals, API keys, plugin asks). It owns NO run state: worker
// threads, busy flags and cancel flags are per-window in RunRegistry.
class EventRouter {
public:
    explicit EventRouter(Tui& tui);

    EventRouter(const EventRouter&) = delete;
    EventRouter& operator=(const EventRouter&) = delete;

    void push(AgentEvent ev);
    std::vector<AgentEvent> pop_all();
    bool empty() const;

    bool shutting_down() const noexcept { return shutting_down_; }
    void set_shutting_down(bool v) noexcept { shutting_down_ = v; }

    std::mutex& mutex() noexcept { return mtx_; }
    std::queue<AgentEvent>& queue() noexcept { return queue_; }

    // `cancel` is the worker's own slot flag (RunSlot::cancel). Hooks read it
    // as a plain atomic — never through RunRegistry, whose map lock is held
    // across joins (a lock here would deadlock quit-while-busy).
    agent::AgentHooks make_hooks(size_t window_id, const std::atomic<bool>& cancel);

    void shutdown_queues(std::queue<AgentEvent>& pending_approvals,
                         std::queue<AgentEvent>& pending_api_keys,
                         std::queue<AgentEvent>& pending_asks);

    std::vector<PendingToolLine>& pending_tools() noexcept { return pending_tools_; }
    std::queue<AgentEvent>& pending_approvals() noexcept { return pending_approvals_; }
    std::queue<AgentEvent>& pending_api_keys() noexcept { return pending_api_keys_; }
    std::queue<AgentEvent>& pending_asks() noexcept { return pending_asks_; }

    // ---- event dispatch (drain_events machinery) -------------------------
    bool drain_events();
    void on_reasoning(Window* w, const AgentEvent& ev);
    void on_token(Window* w, const AgentEvent& ev);
    void on_tool_call(Window* w, const AgentEvent& ev);
    void on_tool_result(Window* w, const AgentEvent& ev);
    void on_assistant(Window* w, const AgentEvent& ev);
    void on_error(Window* w, const AgentEvent& ev);
    void on_done(Window* w, const AgentEvent& ev);
    void on_compress_result(Window* w, const AgentEvent& ev);
    void resolve_approval(const AgentEvent& ev);
    void pump_pending_approvals();
    void resolve_api_key(const AgentEvent& ev);
    void pump_pending_api_keys();
    void resolve_ask(const AgentEvent& ev);
    void pump_pending_asks();
    void advance_tool_spinners();

private:
    size_t find_pending_tool(size_t window_id, const std::string& name,
                             const std::string& fingerprint) const;

    Tui& tui_;
    std::queue<AgentEvent> queue_;
    mutable std::mutex mtx_;
    bool shutting_down_ = false;
    std::vector<PendingToolLine> pending_tools_;
    std::queue<AgentEvent> pending_approvals_;
    std::queue<AgentEvent> pending_api_keys_;
    std::queue<AgentEvent> pending_asks_;
};

} // namespace tui

#endif // AMBER_TUI_EVENT_ROUTER_H

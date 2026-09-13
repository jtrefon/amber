#ifndef AMBER_TUI_RUN_REGISTRY_H
#define AMBER_TUI_RUN_REGISTRY_H

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace tui {

// One window's run machinery: the agent worker thread, its busy flag, its
// cancel flag, and its queue of prompts typed while a run was in flight.
// Owned by RunRegistry, keyed by the window's stable id — a slot's address
// never changes, so worker-side hooks may capture RunSlot* safely.
struct RunSlot {
    std::thread thread;
    std::atomic<bool> busy{false};
    std::atomic<bool> cancel{false};
    std::deque<std::string> pending;
    mutable std::mutex mtx; // guards pending only; flags are atomic
};

// Per-window run slots. "Agent is running" is a per-window fact, not a
// process-global one: each window's agent gets its own worker thread, busy
// flag, cancel flag, and prompt queue. The UI thread owns slot creation and
// erasure; worker threads only touch their own slot's atomics.
class RunRegistry {
public:
    // Fetch (creating on demand) the slot for a window id.
    RunSlot& slot(size_t window_id);

    bool busy(size_t window_id) const;
    bool any_busy() const;
    size_t busy_count() const;

    void request_cancel(size_t window_id);
    bool cancelled(size_t window_id) const;
    void clear_cancel(size_t window_id);

    // Join one window's worker when it is joinable (the UI thread harvests
    // finished workers on the next dispatch to the same window).
    void join(size_t window_id);

    // Shutdown: request cancel on every slot, then join every worker.
    void cancel_all();
    void join_all();

    // Per-window prompt queue — prompts typed while that window's agent is
    // busy wait here and are dispatched when its run finishes.
    void enqueue(size_t window_id, std::string prompt);
    std::optional<std::string> pop_pending(size_t window_id);
    bool has_pending(size_t window_id) const;

    // Drop a slot when its window closes. Callers guarantee the slot is
    // idle (close is rejected while the window's agent runs).
    void erase(size_t window_id);

private:
    mutable std::mutex map_mtx_;
    std::unordered_map<size_t, std::unique_ptr<RunSlot>> slots_;
};

} // namespace tui

#endif // AMBER_TUI_RUN_REGISTRY_H

#include "agent/task_join.h"

#include "agent/event_bus.h"
#include "agent/events.h"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace agent {

namespace {

bool live(const TaskRecord& record) {
    return !task_state_terminal(record.state);
}

std::vector<std::string> live_owned(TaskService& tasks, const std::string& owner) {
    std::vector<std::string> ids;
    for (const auto& record : tasks.list(owner)) {
        if (live(record))
            ids.push_back(record.id);
    }
    return ids;
}

JoinResult collect(TaskService& tasks, const std::vector<std::string>& ids) {
    JoinResult out;
    for (const auto& id : ids) {
        const auto record = tasks.get(id);
        if (!record)
            continue;
        if (live(*record))
            out.still_running.push_back(id);
        else
            out.completed.push_back(*record);
    }
    std::sort(out.completed.begin(), out.completed.end(),
              [](const TaskRecord& a, const TaskRecord& b) {
                  if (a.ended_ms != b.ended_ms)
                      return a.ended_ms > b.ended_ms;
                  return a.id > b.id;
              });
    return out;
}

// A condition variable needs a periodic wake to notice the caller's
// cancellation token: the token has no event source, and a join that only woke
// on task events would sit through a cancel until its deadline.
constexpr std::chrono::milliseconds kTick{50};

} // namespace

JoinResult join_tasks(TaskService& tasks, EventBus& events, const std::string& owner,
                      JoinPolicy policy, const CancellationToken& token) {
    // The event bus copies a handler into its dispatch batch and invokes it
    // without the bus lock, so a subscription cannot be cancelled out of an
    // in-flight fire. The shared state keeps the condition variable alive until
    // the last copied handler returns, even after join_tasks has returned.
    struct State {
        std::mutex mtx;
        std::condition_variable cv;
        std::set<std::string> finished;
    };
    auto state = std::make_shared<State>();
    // Subscribe before the snapshot: a task that finishes between the two would
    // otherwise be terminal without our having seen its event, and the inbox
    // would not be populated yet when the caller drains it.
    auto note = [state](const std::string& id) {
        {
            std::scoped_lock lk(state->mtx);
            state->finished.insert(id);
        }
        state->cv.notify_all();
    };
    const Subscription completed = Events(events).subscribe<TaskCompletedEvent>(
        [note](const TaskCompletedEvent& event) { note(event.id); });
    const Subscription cancelled = Events(events).subscribe<TaskCancelledEvent>(
        [note](const TaskCancelledEvent& event) { note(event.id); });

    const std::vector<std::string> ids = live_owned(tasks, owner);
    if (ids.empty())
        return {};

    const auto all_done = [&] {
        for (const auto& id : ids)
            if (state->finished.count(id) == 0)
                return false;
        return true;
    };
    const auto deadline = std::chrono::steady_clock::now() + policy.deadline;
    std::unique_lock lk(state->mtx);
    while (!all_done() && !token.is_requested()) {
        if (policy.deadline.count() <= 0 || std::chrono::steady_clock::now() >= deadline)
            break;
        state->cv.wait_until(lk, std::min(deadline, std::chrono::steady_clock::now() + kTick));
    }
    lk.unlock();
    return collect(tasks, ids);
}

} // namespace agent

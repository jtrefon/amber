#include "agent/task_join.h"

#include <algorithm>
#include <thread>
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

JoinResult join_tasks(TaskService& tasks, const std::string& owner, JoinPolicy policy,
                      const CancellationToken& token) {
    const std::vector<std::string> ids = live_owned(tasks, owner);
    if (ids.empty())
        return {};

    const auto all_done = [&] {
        return std::all_of(ids.begin(), ids.end(), [&](const std::string& id) {
            const auto record = tasks.get(id);
            return !record || !live(*record);
        });
    };
    const auto deadline = std::chrono::steady_clock::now() + policy.deadline;
    while (!all_done() && !token.is_requested()) {
        if (policy.deadline.count() <= 0 || std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(kTick);
    }
    return collect(tasks, ids);
}

} // namespace agent

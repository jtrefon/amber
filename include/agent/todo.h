
#ifndef AGENT_TODO_H
#define AGENT_TODO_H

// Host-owned task list for the todowrite tool. The model replaces the whole
// list on every call (full-list semantics — the model owns ids and statuses,
// mirroring the TodoWrite contract the strongest agentic models are trained
// on). State lives here, not in context, so it survives compression.

#include <cstdint>
#include <string>
#include <vector>

namespace agent {

enum class TodoStatus : uint8_t {
    Pending,
    InProgress,
    Completed,
    Cancelled,
};

struct TodoItem {
    std::string id;
    std::string text;
    TodoStatus status = TodoStatus::Pending;
};

class TodoStore {
public:
    // Replace the entire list (model sends the full updated list each call).
    void update(std::vector<TodoItem> items) noexcept { items_ = std::move(items); }

    const std::vector<TodoItem>& items() const noexcept { return items_; }

private:
    std::vector<TodoItem> items_;
};

} // namespace agent

#endif // AGENT_TODO_H

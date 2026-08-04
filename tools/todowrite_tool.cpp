
#include "agent/tools.h"

#include <sstream>

#include "agent/todo.h"

namespace agent {

namespace {

const char* status_name(TodoStatus s) noexcept {
    switch (s) {
        case TodoStatus::Pending: return "pending";
        case TodoStatus::InProgress: return "in_progress";
        case TodoStatus::Completed: return "completed";
        case TodoStatus::Cancelled: return "cancelled";
    }
    return "pending";
}

bool parse_status(const std::string& s, TodoStatus& out) noexcept {
    if (s == "pending") out = TodoStatus::Pending;
    else if (s == "in_progress") out = TodoStatus::InProgress;
    else if (s == "completed") out = TodoStatus::Completed;
    else if (s == "cancelled") out = TodoStatus::Cancelled;
    else return false;
    return true;
}

} // namespace

class TodowriteTool : public Tool {
public:
    explicit TodowriteTool(TodoStore& store) : store_(store) {}

    std::string name() const noexcept override { return "todowrite"; }

    std::string description() const noexcept override {
        return "Create and maintain a structured task list for the current "
               "session. Tracks progress during multi-step work and keeps "
               "status visible to the user. The full list is sent on every "
               "call and replaces the previous one. Item statuses: pending, "
               "in_progress, completed, cancelled.";
    }

    agent::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"todos",
               {{"type", "array"},
                {"items",
                 {{"type", "object"},
                  {"properties",
                   {{"id", {{"type", "string"}}},
                    {"text", {{"type", "string"}}},
                    {"status",
                     {{"type", "string"},
                      {"enum", {"pending", "in_progress", "completed",
                                "cancelled"}}}}}},
                  {"required", {"id", "text", "status"}}}}}}}},
            {"required", {"todos"}}};
    }

    bool is_read_only() const noexcept override { return false; }

    ToolResult execute(const agent::json& args) const override {
        if (!args.contains("todos") || !args["todos"].is_array())
            return ToolResult{false, "", "todowrite: missing todos array",
                              agent::json::object()};
        std::vector<TodoItem> items;
        for (const auto& e : args["todos"]) {
            if (!e.is_object() || !e.contains("id") || !e["id"].is_string() ||
                !e.contains("text") || !e["text"].is_string() ||
                !e.contains("status") || !e["status"].is_string())
                return ToolResult{false, "",
                                  "todowrite: each todo needs id, text, status",
                                  agent::json::object()};
            TodoStatus st;
            if (!parse_status(e["status"].get<std::string>(), st))
                return ToolResult{false, "",
                                  "todowrite: invalid status (pending | "
                                  "in_progress | completed | cancelled)",
                                  agent::json::object()};
            items.push_back({e["id"].get<std::string>(),
                             e["text"].get<std::string>(), st});
        }
        store_.update(std::move(items));
        std::ostringstream out;
        for (const auto& t : store_.items())
            out << t.id << " [" << status_name(t.status) << "] " << t.text
                << "\n";
        return ToolResult{true, out.str(), "", agent::json::object()};
    }

private:
    TodoStore& store_;
};

std::unique_ptr<Tool> make_todowrite_tool(TodoStore& todos) {
    return std::make_unique<TodowriteTool>(todos);
}

} // namespace agent

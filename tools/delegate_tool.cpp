#include "agent/tools.h"

#include <string>

#include "agent/subagent.h"
#include "agent/task.h"

namespace agent {

namespace {

constexpr std::size_t kFieldCap = std::size_t{16} * 1024;
constexpr std::size_t kLabelCap = 128;

std::string field(const json& args, const char* name) {
    if (!args.contains(name) || !args[name].is_string())
        return {};
    return args[name].get<std::string>();
}

class DelegateTool : public Tool {
public:
    DelegateTool(SubAgentExecutor& executor, ToolRegistry& registry)
        : executor_(executor), registry_(registry) {}

    std::string name() const noexcept override { return "delegate"; }

    std::string description() const noexcept override {
        return "Launch a detached worker agent with fresh context. The worker "
               "does not see this conversation, so include everything it needs. "
               "The call returns a task id immediately; the worker runs in the "
               "background and its bounded report arrives when it finishes. "
               "Use tasks status/output/cancel/wait to inspect or control it. "
               "Delegate self-contained surveys, research or isolated edits "
               "instead of doing them inline.";
    }

    agent::json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"goal",
                   {{"type", "string"},
                    {"description", "What the worker must do and verify. Required."}}},
                  {"context",
                   {{"type", "string"},
                    {"description", "Everything the worker needs: files, paths, constraints. "
                                    "The worker does not see the parent conversation. Required."}}},
                  {"expect",
                   {{"type", "string"},
                    {"description", "The report shape and success criteria. Required."}}},
                  {"label", {{"type", "string"}, {"description", "Short human label for /tasks."}}},
                  {"max_turns",
                   {{"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 1000},
                    {"description", "Optional bound on the worker's tool loop."}}}}},
                {"required", {"goal", "context", "expect"}}};
    }

    // The worker inherits the parent's full tool registry — a model denied
    // bash could spawn a worker whose tool runs it ungated. Always prompt.
    bool requires_approval(const json&) const noexcept override { return true; }

    std::string summarize(const json& args) const override {
        const std::string label = args.value("label", std::string());
        return label.empty() ? "delegate" : "delegate \"" + label + "\"";
    }

    ToolResult execute(const agent::json& args) const override {
        if (in_subagent())
            return ToolResult{false, "", "delegate cannot be nested inside a worker",
                              agent::json::object()};
        SpawnRequest request;
        request.goal = field(args, "goal");
        request.context = field(args, "context");
        request.expect = field(args, "expect");
        request.label = field(args, "label");
        if (request.goal.size() > kFieldCap || request.context.size() > kFieldCap ||
            request.expect.size() > kFieldCap)
            return ToolResult{false, "",
                              "delegate: goal, context and expect are each limited to 16 KiB",
                              agent::json::object()};
        if (request.label.size() > kLabelCap)
            request.label.resize(kLabelCap);
        if (args.contains("max_turns") && args["max_turns"].is_number_integer())
            request.max_turns = args["max_turns"].get<int>();
        request.owner = task_owner_from_scope();

        std::string err;
        const std::string id = executor_.spawn(std::move(request), registry_, err);
        if (id.empty())
            return ToolResult{false, "", err.empty() ? "delegate: spawn failed" : err,
                              agent::json::object()};
        return ToolResult{true, id, "", {{"task_id", id}}};
    }

private:
    SubAgentExecutor& executor_;
    ToolRegistry& registry_;
};

} // namespace

std::unique_ptr<Tool> make_delegate_tool(SubAgentExecutor& executor, ToolRegistry& registry) {
    return std::make_unique<DelegateTool>(executor, registry);
}

} // namespace agent

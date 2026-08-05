
#include "agent/tools.h"

#include <string>

#include "agent/subagent.h"

namespace agent {

namespace {

class TaskTool : public Tool {
public:
    TaskTool(SubAgentExecutor& executor, ToolRegistry& registry)
        : executor_(executor), registry_(registry) {}

    std::string name() const noexcept override { return "task"; }

    std::string description() const noexcept override {
        return "Launch a focused worker agent to handle a task autonomously. "
               "The worker gets its own context and completes the task with "
               "the available tools, returning a concise report. Delegate "
               "self-contained subtasks (exploration, research, isolated "
               "edits) instead of doing them inline; the worker's result "
               "comes back as this tool's output. Execution is serial or "
               "parallel per the subagent settings.";
    }

    agent::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"prompt",
               {{"type", "string"},
                {"description",
                 "The focused task for the worker agent: what to do, what "
                 "to verify, and what to report back."}}}}},
            {"required", {"prompt"}}};
    }

    ToolResult execute(const agent::json& args) const override {
        if (!args.contains("prompt") || !args["prompt"].is_string())
            return ToolResult{false, "", "task: missing prompt",
                              agent::json::object()};
        if (in_subagent())
            return ToolResult{false,
                              "",
                              "task cannot be nested inside a sub-agent",
                              agent::json::object()};
        std::string err;
        std::string out =
            executor_.run_task(args["prompt"].get<std::string>(), registry_,
                               err);
        if (!err.empty())
            return ToolResult{false, "", err, agent::json::object()};
        return ToolResult{true, out, "", agent::json::object()};
    }

private:
    SubAgentExecutor& executor_;
    ToolRegistry& registry_;
};

} // namespace

std::unique_ptr<Tool> make_task_tool(SubAgentExecutor& executor,
                                     ToolRegistry& registry) {
    return std::make_unique<TaskTool>(executor, registry);
}

} // namespace agent

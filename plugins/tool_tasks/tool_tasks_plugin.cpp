#include "tool_tasks_plugin.h"

#include "agent/task.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_tasks_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "tasks",
        [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!services.host || !services.host->tasks || !services.host->jobs)
                return {};
            const CancellationToken fallback =
                services.host->cancel_token ? *services.host->cancel_token : CancellationToken{};
            return wrap_tool(
                make_tasks_tool(*services.host->tasks, *services.host->jobs, fallback));
        },
        ToolCapability::Meta{{{"tasks", {"tasks", ToolRole::Control}}}}));
    caps.push_back(
        make_tool_doc_capability("tasks_doc", tool_doc_priority::kTasks, "prompts/tools/tasks.md"));
    return caps;
}

} // namespace agent::plugins

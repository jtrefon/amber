#include "tool_task_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_task_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "task",
        [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!services.config || !services.config->task_tool)
                return {};
            if (!services.host || !services.host->subagents)
                return {};
            return wrap_tool(make_task_tool(*services.host->subagents, services.tools()));
        },
        ToolCapability::Verbs{{{"task", "delegating"}}}));
    return caps;
}

} // namespace agent::plugins

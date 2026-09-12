#include "tool_plan_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_plan_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // Gated on the host's config: absent rather than erroring when off. The
    // plugin can be switched off as well - two ways to the same state, until
    // the config flag retires and plugin state is the only one left.
    caps.push_back(std::make_unique<ToolCapability>(
        "todowrite",
        [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!services.config || !services.config->plan_tool)
                return {};
            if (!services.host || !services.host->todos)
                return {};
            return wrap_tool(make_todowrite_tool(*services.host->todos));
        },
        ToolCapability::Verbs{{{"todowrite", "planning"}}}));
    return caps;
}

} // namespace agent::plugins

#include "tool_plan_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_plan_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "todowrite",
        [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!services.host || !services.host->todos)
                return {};
            return wrap_tool(make_todowrite_tool(*services.host->todos));
        },
        ToolCapability::Meta{{{"todowrite", {"planning", ToolRole::Plan}}}}));
    // The prose that describes the tool travels with it: switching this plugin
    // off removes the tool and the text that would have taught the model to use
    // it, in one movement.
    caps.push_back(
        make_tool_doc_capability("plan_doc", tool_doc_priority::kPlan, "prompts/tools/plan.md"));
    return caps;
}

} // namespace agent::plugins

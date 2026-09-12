#include "tool_bash_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_bash_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "bash",
        [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            JobService* jobs = services.host ? services.host->jobs : nullptr;
            const CancellationToken token = (services.host && services.host->cancel_token)
                                                ? *services.host->cancel_token
                                                : CancellationToken{};
            return wrap_tool(make_bash_tool(jobs, token));
        },
        ToolCapability::Verbs{{{"bash", "hacking"}}}));
    caps.push_back(
        make_tool_doc_capability("bash_doc", tool_doc_priority::kBash, "prompts/tools/bash.md"));
    return caps;
}

} // namespace agent::plugins

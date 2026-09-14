#include "tool_search_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_search_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "search",
        [](PluginServices& services) {
            // The runtime's live registry, through a provider that keeps it
            // alive: the tool resolves whatever backends are enabled now.
            return wrap_tool(make_search_tool(services.search_backend_provider()));
        },
        ToolCapability::Meta{{{"search", {"searching", ToolRole::Search}}}}));
    caps.push_back(make_tool_doc_capability("search_doc", tool_doc_priority::kSearch,
                                            "prompts/tools/search.md"));
    return caps;
}

} // namespace agent::plugins


#include "agent/extensions.h"
#include "agent/job.h"
#include "agent/process.h"
#include "agent/registry.h"
#include "agent/todo.h"
#include "agent/tools.h"
#include "plugins/tool_bash/tool_bash_plugin.h"
#include "plugins/tool_plan/tool_plan_plugin.h"
#include "plugins/tool_process/tool_process_plugin.h"
#include "plugins/tool_read/tool_read_plugin.h"
#include "plugins/tool_search/tool_search_plugin.h"
#include "plugins/tool_task/tool_task_plugin.h"
#include "plugins/tool_write/tool_write_plugin.h"

namespace agent {

std::vector<std::unique_ptr<Tool>> wrap_tool(std::unique_ptr<Tool> tool) {
    std::vector<std::unique_ptr<Tool>> tools;
    if (tool)
        tools.push_back(std::move(tool));
    return tools;
}

// Register the built-in tools. The set itself lives in one plugin per tunable
// unit (plugins/tool_*), which is where a host that runs the plugin lifecycle
// gets them. This entry point installs the same capabilities directly for hosts
// and tests that hold a bare registry and no runtime - the definitions are the
// plugins' own, so the two paths cannot drift.
//
// `prompts` is where the tool documentation blocks go. Without one the tools
// are installed undocumented, which is what a bare unit test wants; the hosts
// pass their registry so a tool and its prose always arrive together.
void register_default_tools(ToolRegistry& reg, JobService& jobs, TodoStore& todos,
                            const CancellationToken& cancel_token, SubAgentExecutor& subagents,
                            PromptRegistry* prompts) {
    Config cfg;
    HostServices host{&jobs, &todos, &subagents, &cancel_token};

    // Tool capabilities reach only services.tools() and services.config/host,
    // so the registries the tool set does not use are empty placeholders.
    EventBus events;
    PromptRegistry scratch_prompts;
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    PluginServices services(reg, prompts ? *prompts : scratch_prompts, status, panels, wallets,
                            events);
    services.config = &cfg;
    services.host = &host;

    std::vector<std::unique_ptr<Capability>> declared;
    auto append = [&declared](std::vector<std::unique_ptr<Capability>> more) {
        for (auto& cap : more)
            declared.push_back(std::move(cap));
    };
    append(plugins::make_search_tool_capabilities());
    append(plugins::make_read_tool_capabilities());
    append(plugins::make_write_tool_capabilities());
    append(plugins::make_bash_tool_capabilities());
    append(plugins::make_process_tool_capabilities());
    append(plugins::make_plan_tool_capabilities());
    append(plugins::make_task_tool_capabilities());

    for (auto& capability : declared) {
        InstallResult r = capability->install(services);
        (void)r;
    }
}

} // namespace agent

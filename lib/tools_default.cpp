
#include "agent/registry.h"
#include "agent/tools.h"
#include "agent/process.h"
#include "agent/job.h"
#include "agent/todo.h"
#include "agent/extensions.h"
#include "plugins/core_tools/core_tools_plugin.h"

namespace agent {

// Register the built-in tools. The set itself lives in the core_tools plugin
// (make_core_tool_capabilities), which is where a host that runs the plugin
// lifecycle gets them. This entry point installs the same capabilities directly
// for hosts and tests that hold a bare registry and no runtime — one
// definition, so the two paths cannot drift.
void register_default_tools(ToolRegistry& reg, JobService& jobs, TodoStore& todos,
                            const CancellationToken& cancel_token, bool enable_plan_tool,
                            SubAgentExecutor& subagents, bool enable_task_tool) {
    Config cfg;
    cfg.plan_tool = enable_plan_tool;
    cfg.task_tool = enable_task_tool;
    HostServices host{&jobs, &todos, &subagents, &cancel_token};

    // Tool capabilities reach only services.tools() and services.config/host, so
    // the registries the core tool set does not use are empty placeholders.
    EventBus events;
    PromptRegistry prompts;
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    AllowanceRegistry allowances;
    PluginServices services(reg, prompts, status, panels, wallets, allowances, events);
    services.config = &cfg;
    services.host = &host;

    for (auto& capability : plugins::make_core_tool_capabilities()) {
        InstallResult r = capability->install(services);
        (void)r;
    }
}

} // namespace agent


#include "agent/extensions.h"
#include "agent/job.h"
#include "agent/process.h"
#include "agent/registry.h"
#include "agent/todo.h"
#include "agent/tools.h"
#include "plugins/search_grep/search_grep_plugin.h"
#include "plugins/search_semantic/search_semantic_plugin.h"
#include "plugins/tool_bash/tool_bash_plugin.h"
#include "plugins/tool_plan/tool_plan_plugin.h"
#include "plugins/tool_process/tool_process_plugin.h"
#include "plugins/tool_read/tool_read_plugin.h"
#include "plugins/tool_search/tool_search_plugin.h"
#include "plugins/tool_task/tool_task_plugin.h"
#include "plugins/tool_write/tool_write_plugin.h"

namespace agent {

namespace {

// The registries a host needs when it holds a bare tool registry and no
// PluginRuntime. Only tools, prompts and search backends are touched by the
// capabilities installed this way; the rest exist to satisfy PluginServices and
// stay empty. The search-backend table is shared with whatever provider is
// handed out, so it outlives the install (search_backend.h).
struct BareHost {
    PromptRegistry prompts;
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    EventBus events;
    CommandRegistry commands;
    std::shared_ptr<SearchBackendRegistry> search_backends =
        std::make_shared<SearchBackendRegistry>();
    PluginServices services;

    // `prompt_sink` is where tool documentation blocks go; without one they are
    // installed nowhere, which is what a bare unit test wants.
    explicit BareHost(ToolRegistry& tools, PromptRegistry* prompt_sink = nullptr)
        : services(tools, prompt_sink ? *prompt_sink : prompts, status, panels, wallets,
                   search_backends, events, commands) {}
};

// Install a set of capabilities through the same path the runtime uses. A failed
// install in a bare host has no plugin to fail: the caller holds no ledger, so
// the result is deliberately ignored here exactly as it was before the
// capability path existed.
void install_capabilities(BareHost& host, std::vector<std::unique_ptr<Capability>> caps) {
    for (auto& capability : caps) {
        if (!capability)
            continue;
        InstallResult result = capability->install(host.services);
        (void)result;
    }
}

} // namespace

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
    BareHost bare(reg, prompts);
    bare.services.config = &cfg;
    bare.services.host = &host;

    install_capabilities(bare, plugins::make_search_tool_capabilities());
    install_capabilities(bare, plugins::make_read_tool_capabilities());
    install_capabilities(bare, plugins::make_write_tool_capabilities());
    install_capabilities(bare, plugins::make_bash_tool_capabilities());
    install_capabilities(bare, plugins::make_process_tool_capabilities());
    install_capabilities(bare, plugins::make_plan_tool_capabilities());
    install_capabilities(bare, plugins::make_task_tool_capabilities());
    // The search backends, from the same definitions the bundled plugins
    // declare: a bare host's search tool resolves the shipped pair.
    install_capabilities(bare, plugins::make_grep_backend_capabilities());
    install_capabilities(bare, plugins::make_semantic_backend_capabilities());
}

SearchBackendProvider builtin_search_backend_provider() {
    // The backends amber ships, without a runtime: the same capabilities the
    // bundled backend plugins declare, installed into a private registry the
    // returned provider keeps alive (search_backend.h).
    ToolRegistry scratch_tools;
    BareHost bare(scratch_tools);
    install_capabilities(bare, plugins::make_grep_backend_capabilities());
    install_capabilities(bare, plugins::make_semantic_backend_capabilities());
    return make_search_backend_provider(bare.search_backends);
}

} // namespace agent

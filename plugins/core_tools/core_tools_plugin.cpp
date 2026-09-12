#include "core_tools_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tools.h"

namespace agent::plugins {

namespace {

// The config flags arrive through the services; a null config means a host that
// attached none (tests), where the optional tools stay off.
bool flag(const PluginServices& services, bool (*pick)(const Config&)) {
    return services.config && pick(*services.config);
}

// A factory hands back a list; most capabilities contribute exactly one tool.
std::vector<std::unique_ptr<Tool>> wrap(std::unique_ptr<Tool> tool) {
    std::vector<std::unique_ptr<Tool>> tools;
    if (tool)
        tools.push_back(std::move(tool));
    return tools;
}

} // namespace

std::vector<std::unique_ptr<Capability>> make_core_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;

    // Tools with no dependencies.
    caps.push_back(std::make_unique<ToolCapability>(
        "read", [](PluginServices&) { return wrap(make_read_tool()); }));
    caps.push_back(std::make_unique<ToolCapability>(
        "write", [](PluginServices&) { return wrap(make_write_tool()); }));
    caps.push_back(std::make_unique<ToolCapability>(
        "search", [](PluginServices&) { return wrap(make_search_tool()); }));

    // Gated on configuration: absent rather than erroring when the flag is off.
    caps.push_back(std::make_unique<ToolCapability>(
        "todowrite", [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!flag(services, [](const Config& c) { return c.plan_tool; }))
                return {};
            if (!services.host || !services.host->todos)
                return {};
            return wrap(make_todowrite_tool(*services.host->todos));
        }));
    caps.push_back(std::make_unique<ToolCapability>(
        "task", [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!flag(services, [](const Config& c) { return c.task_tool; }))
                return {};
            if (!services.host || !services.host->subagents)
                return {};
            return wrap(make_task_tool(*services.host->subagents, services.tools()));
        }));

    // Shell: binds to the job service and the cancel token.
    caps.push_back(std::make_unique<ToolCapability>(
        "bash", [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            JobService* jobs = services.host ? services.host->jobs : nullptr;
            const CancellationToken token = (services.host && services.host->cancel_token)
                                                ? *services.host->cancel_token
                                                : CancellationToken{};
            return wrap(make_bash_tool(jobs, token));
        }));

    // The process tools are several tools behind one binding, so they are one
    // contribution: one ledger entry, removed together.
    caps.push_back(std::make_unique<ToolCapability>(
        "process", [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!services.host || !services.host->jobs)
                return {};
            return make_process_tools(*services.host->jobs);
        }));
    return caps;
}

} // namespace agent::plugins

#ifndef AGENT_PLUGINS_TOOL_TOOL_TASKS_H
#define AGENT_PLUGINS_TOOL_TOOL_TASKS_H

#include "agent/extensions.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::plugins {

// The tool capabilities this plugin contributes. Exposed as a free function so
// a host with no plugin runtime (register_default_tools) can install exactly
// the same set: one definition, two install paths.
std::vector<std::unique_ptr<Capability>> make_tasks_tool_capabilities();

class ToolTasksPlugin : public IPlugin {
public:
    std::string id() const override { return "tool_tasks"; }
    std::string version() const override { return "0.1.0"; }
    std::string name() const override { return "Task tools"; }
    std::string description() const override {
        return "One inventory for background work: list, inspect, output, cancel and wait.";
    }
    std::string category() const override { return plugin_category::kTools; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        return make_tasks_tool_capabilities();
    }
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_TOOL_TOOL_TASKS_H

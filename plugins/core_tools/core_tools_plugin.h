#ifndef AGENT_PLUGINS_CORE_TOOLS_H
#define AGENT_PLUGINS_CORE_TOOLS_H

// The core tool set as a plugin contribution.
//
// These tools are core harness functionality, but they are *contributed*
// through the same capability path a third-party tool would use — the plugin
// declares a `ToolCapability` per group, and the runtime installs it. That is
// the point: the path the core uses is the path an extension uses, so it cannot
// rot.
//
// The tools need host-owned services to construct (the job service, the todo
// store, the sub-agent executor, the cancel token), which is what
// `attach_host_services` provides. A capability whose tool is gated on
// configuration simply declines, so `todowrite` and `task` are absent unless
// the matching flag is on.

#include "agent/extensions.h"
#include "agent/plugin_v2.h"

#include <memory>
#include <vector>

namespace agent::plugins {

// The tool capabilities, built from whatever services the runtime carries. One
// definition, so the plugin path and the direct-install convenience below
// cannot drift.
std::vector<std::unique_ptr<Capability>> make_core_tool_capabilities();

class CoreToolsPlugin : public IPlugin {
public:
    std::string id() const override { return "core_tools"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Core tools"; }
    std::string description() const override {
        return "The built-in file, search, shell and process tools.";
    }
    std::string category() const override { return plugin_category::kTools; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override {
        return make_core_tool_capabilities();
    }
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_CORE_TOOLS_H

#ifndef AGENT_PLUGINS_TOOL_TOOL_RATE_H
#define AGENT_PLUGINS_TOOL_TOOL_RATE_H

#include "agent/extensions.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::plugins {

// The rate-control tools: `wait` (one-off pause) and `throttle` (inspect or
// cap the provider governor). One definition, installed by the plugin runtime
// and by the bare-host path in register_default_tools.
std::vector<std::unique_ptr<Capability>> make_rate_tool_capabilities();

class ToolRatePlugin : public IPlugin {
public:
    std::string id() const override { return "tool_rate"; }
    std::string version() const override { return "0.5.12"; }
    std::string name() const override { return "Rate-control tools"; }
    std::string description() const override {
        return "Wait and throttle tools: pause a turn or inspect/cap provider pacing.";
    }
    std::string category() const override { return plugin_category::kTools; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        return make_rate_tool_capabilities();
    }
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_TOOL_TOOL_RATE_H

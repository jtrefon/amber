#ifndef AGENT_PLUGINS_TOOL_TOOL_WRITE_H
#define AGENT_PLUGINS_TOOL_TOOL_WRITE_H

#include "agent/extensions.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::plugins {

// The tool capabilities this plugin contributes. Exposed as a free function so
// a host with no plugin runtime (register_default_tools) can install exactly
// the same set: one definition, two install paths.
std::vector<std::unique_ptr<Capability>> make_write_tool_capabilities();

class ToolWritePlugin : public IPlugin {
public:
    std::string id() const override { return "tool_write"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Write tool"; }
    std::string description() const override {
        return "The write tool: targeted replace-block edits to files.";
    }
    std::string category() const override { return plugin_category::kTools; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        return make_write_tool_capabilities();
    }
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_TOOL_TOOL_WRITE_H

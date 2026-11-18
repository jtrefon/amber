#ifndef AGENT_PLUGINS_TOOL_CHECKPOINT_H
#define AGENT_PLUGINS_TOOL_CHECKPOINT_H

#include "agent/extensions.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::plugins {

// The checkpoint tool and its prompt documentation. Exposed as a free function
// so register_default_tools installs the same capability the bundled plugin
// declares, and the two paths cannot drift.
std::vector<std::unique_ptr<Capability>> make_checkpoint_tool_capabilities();

class ToolCheckpointPlugin : public IPlugin {
public:
    std::string id() const override { return "tool_checkpoint"; }
    std::string version() const override { return "0.1.0"; }
    std::string name() const override { return "Checkpoint tool"; }
    std::string description() const override {
        return "The git_checkpoint tool: list, diff and restore amber's per-turn snapshots.";
    }
    std::string category() const override { return plugin_category::kTools; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        return make_checkpoint_tool_capabilities();
    }
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_TOOL_CHECKPOINT_H

#ifndef AGENT_PLUGIN_EXTERNAL_ADAPTER_H
#define AGENT_PLUGIN_EXTERNAL_ADAPTER_H

// Brings the v1 external plugin tier under the runtime (spec §2, D17).
//
// External plugins are separate processes discovered from the plugin
// directories; they contribute tools and nothing else. The adapter gives each
// one the same lifecycle surface as a bundled plugin, so `/get plugin list`
// shows both tiers and `/set plugin <id> on|off` is the single write path for
// either. Without this the two tiers would need two control surfaces, which is
// exactly the drift the get/set rule exists to prevent.

#include "agent/plugin.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent {

// Wraps one discovered external plugin. The process tier contributes tools
// through the same owner-scoped ToolSourceCapability path a bundled plugin
// uses, so the runtime ledger owns the tool set and deactivation unwinds
// exactly what this plugin registered. The process is spawned lazily on the
// first tool call, never during activation.
class ExternalPluginAdapter : public IPlugin {
public:
    ExternalPluginAdapter(PluginManager& manager, std::string plugin_id, std::string version);

    std::string id() const override { return id_; }
    std::string version() const override { return version_; }
    std::string name() const override { return "External plugin " + id_; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override;
    std::vector<std::unique_ptr<Capability>> capabilities() override;

private:
    PluginManager* manager_;
    std::string id_;
    std::string version_;
    bool initialized_ = false;
};

// Adapters for every plugin the manager has discovered. Built at startup; a
// plugin installed later is picked up by rebuilding the runtime's plugin list
// (the install command already re-runs discovery).
std::vector<std::shared_ptr<IPlugin>> make_v1_plugin_adapters(PluginManager& manager);

} // namespace agent

#endif // AGENT_PLUGIN_EXTERNAL_ADAPTER_H


#ifndef AGENT_PLUGIN_V2_H
#define AGENT_PLUGIN_V2_H

#include "agent/config.h"
#include "agent/event_bus.h"
#include "agent/plugin_capability.h"
#include "agent/registry.h"
#include "agent/workspace.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agent {

struct PluginContext {
    EventBus& event_bus;
    const ToolRegistry& tools;
    const Config& config;
    const Workspace& workspace;
};

class IPlugin {
public:
    virtual ~IPlugin() = default;

    virtual std::string id() const = 0;
    virtual std::string version() const = 0;
    virtual std::string name() const = 0;

    virtual bool initialize(const PluginContext& ctx) = 0;
    virtual void shutdown() = 0;

    // What this plugin contributes. Called once, at activation: the runtime
    // installs each capability and records the returned handle in its ledger,
    // so the plugin hands over ownership and never registers anything itself.
    // The default is a plugin that contributes nothing yet still participates
    // in the lifecycle (the metrics observer is one).
    virtual std::vector<std::unique_ptr<Capability>> capabilities() { return {}; }
};

} // namespace agent

#endif // AGENT_PLUGIN_V2_H

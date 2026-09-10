
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
    // The host's LIVE configuration, not a copy: a plugin that reads an API
    // key or the active provider must see what the user has changed since
    // startup. The runtime rebinds this when the host hands it the real
    // config (PluginRuntime::attach_config).
    const Config* config = nullptr;
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

    // Called periodically by the host on its UI tick, for time-driven work
    // (polling a balance, refreshing a remote value). It exists so that
    // *rendering* can stay a pure read: a segment never fetches anything, the
    // tick does the work and the segment reports the cached result.
    // Must not block — schedule slow work on the plugin's own thread.
    virtual void tick() {}

    // What this plugin contributes. Called once, at activation: the runtime
    // installs each capability and records the returned handle in its ledger,
    // so the plugin hands over ownership and never registers anything itself.
    // The default is a plugin that contributes nothing yet still participates
    // in the lifecycle (the metrics observer is one).
    virtual std::vector<std::unique_ptr<Capability>> capabilities() { return {}; }
};

} // namespace agent

#endif // AGENT_PLUGIN_V2_H

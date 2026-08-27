
#ifndef AGENT_PLUGIN_V2_H
#define AGENT_PLUGIN_V2_H

#include "agent/config.h"
#include "agent/event_bus.h"
#include "agent/registry.h"
#include "agent/workspace.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agent {

struct Capability {
    enum class Type : std::uint8_t {
        Tool,
        Provider,
        Completion,
        Hook,
        Theme,
        PromptSource,
        Memory,
        Search,
    };

    Type type;
    std::string name;
    std::string description;
    void* impl = nullptr;
};

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

    virtual std::vector<Capability> capabilities() const = 0;
};

} // namespace agent

#endif // AGENT_PLUGIN_V2_H

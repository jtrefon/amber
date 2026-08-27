
#ifndef AGENT_PLUGIN_REGISTRY_H
#define AGENT_PLUGIN_REGISTRY_H

#include "agent/config.h"
#include "agent/event_bus.h"
#include "agent/plugin_v2.h"
#include "agent/registry.h"
#include "agent/workspace.h"

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace agent {

class PluginRegistry {
public:
    enum class State : std::uint8_t {
        Discovered,
        Registered,
        Active,
        Failed,
        Deactivated,
        Shutdown,
    };

    friend std::ostream& operator<<(std::ostream& os, State s) {
        switch (s) {
            case State::Discovered: return os << "Discovered";
            case State::Registered: return os << "Registered";
            case State::Active: return os << "Active";
            case State::Failed: return os << "Failed";
            case State::Deactivated: return os << "Deactivated";
            case State::Shutdown: return os << "Shutdown";
        }
        return os << "Unknown";
    }

    struct PluginInfo {
        std::string id;
        State state = State::Discovered;
    };

    void register_plugin(std::shared_ptr<IPlugin> plugin);
    bool activate(const std::string& id);
    bool deactivate(const std::string& id);
    void shutdown_all();

    State state(const std::string& id) const;
    std::vector<PluginInfo> list() const;
    std::shared_ptr<IPlugin> find(const std::string& id) const;

    EventBus& event_bus();
    void set_context(PluginContext* ctx);
    const PluginContext& context() const;

private:
    struct Entry {
        std::shared_ptr<IPlugin> plugin;
        State state = State::Discovered;
    };

    std::map<std::string, Entry> entries_;
    EventBus bus_;
    PluginContext* ctx_ = nullptr;
};

} // namespace agent

#endif // AGENT_PLUGIN_REGISTRY_H

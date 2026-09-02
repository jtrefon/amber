
#include "agent/plugin_registry.h"

#include <cassert>

namespace agent {

void PluginRegistry::register_plugin(std::shared_ptr<IPlugin> plugin) {
    Entry e;
    e.plugin = std::move(plugin);
    e.state = State::Registered;
    entries_[e.plugin->id()] = std::move(e);
}

bool PluginRegistry::activate(const std::string& id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (it->second.state == State::Active) return true;
    if (!ctx_) {
        return false;
    }
    bool ok = it->second.plugin->initialize(*ctx_);
    it->second.state = ok ? State::Active : State::Failed;
    return ok;
}

bool PluginRegistry::deactivate(const std::string& id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (it->second.state != State::Active) return false;

    it->second.plugin->shutdown();
    it->second.state = State::Deactivated;
    return true;
}

void PluginRegistry::shutdown_all() {
    for (auto& [id, entry] : entries_) {
        if (entry.state == State::Active) {
            entry.plugin->shutdown();
            entry.state = State::Shutdown;
        }
    }
}

PluginRegistry::State PluginRegistry::state(const std::string& id) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) return State::Discovered;
    return it->second.state;
}

std::vector<PluginRegistry::PluginInfo> PluginRegistry::list() const {
    std::vector<PluginInfo> out;
    out.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        out.push_back({id, entry.state});
    }
    return out;
}

std::shared_ptr<IPlugin> PluginRegistry::find(const std::string& id) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) return nullptr;
    return it->second.plugin;
}

EventBus& PluginRegistry::event_bus() { return bus_; }

void PluginRegistry::set_context(PluginContext* ctx) { ctx_ = ctx; }

const PluginContext& PluginRegistry::context() const {
    assert(ctx_ && "PluginRegistry::context called before set_context()");
    return *ctx_;
}

} // namespace agent

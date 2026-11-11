
#include "agent/plugin_registry.h"

namespace agent {

void PluginRegistry::register_plugin(std::shared_ptr<IPlugin> plugin) {
    Entry e;
    e.plugin = std::move(plugin);
    e.state = State::Registered;
    entries_[e.plugin->id()] = std::move(e);
}

bool PluginRegistry::activate(const std::string& id) {
    auto it = entries_.find(id);
    if (it == entries_.end())
        return false;
    if (it->second.state == State::Active)
        return true;
    if (!ctx_) {
        return false;
    }
    bool ok = false;
    try {
        ok = it->second.plugin->initialize(*ctx_);
    } catch (...) {
        // A plugin is extension code: a throw during activation is a failed
        // plugin, not a failed harness.
        ok = false;
    }
    it->second.state = ok ? State::Active : State::Failed;
    return ok;
}

bool PluginRegistry::deactivate(const std::string& id) {
    auto it = entries_.find(id);
    if (it == entries_.end())
        return false;
    if (it->second.state != State::Active)
        return false;

    try {
        it->second.plugin->shutdown();
    } catch (...) {
        // Teardown must not abort the ledger unwind that follows.
    }
    it->second.state = State::Deactivated;
    return true;
}

void PluginRegistry::unregister_plugin(const std::string& id) {
    entries_.erase(id);
}

PluginRegistry::State PluginRegistry::state(const std::string& id) const {
    auto it = entries_.find(id);
    if (it == entries_.end())
        return State::Discovered;
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
    if (it == entries_.end())
        return nullptr;
    return it->second.plugin;
}

void PluginRegistry::set_context(PluginContext* ctx) {
    ctx_ = ctx;
}

} // namespace agent

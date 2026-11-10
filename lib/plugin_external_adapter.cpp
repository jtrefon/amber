#include "agent/plugin_external_adapter.h"

#include "agent/extensions.h"

namespace agent {

ExternalPluginAdapter::ExternalPluginAdapter(PluginManager& manager, std::string plugin_id,
                                             std::string version)
    : manager_(&manager), id_(std::move(plugin_id)), version_(std::move(version)) {}

bool ExternalPluginAdapter::initialize(const PluginContext&) {
    const PluginInfo* info = manager_->find(id_);
    if (!info || info->state == PluginState::Incompatible)
        return false;
    initialized_ = true;
    return true;
}

void ExternalPluginAdapter::shutdown() {
    if (!initialized_)
        return;
    manager_->disable(id_);
    initialized_ = false;
}

std::vector<std::unique_ptr<Capability>> ExternalPluginAdapter::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // The manager registers the manifest's tools through the port. The runtime
    // owns the port and tags every registration with this plugin's id, so a
    // disable removes exactly this set and no other plugin's tools.
    caps.push_back(std::make_unique<ToolSourceCapability>(
        "tools", [this](PluginServices&, ToolPort& port) { manager_->enable(id_, port); },
        [this] { manager_->disable(id_); }));
    return caps;
}

std::vector<std::shared_ptr<IPlugin>> make_v1_plugin_adapters(PluginManager& manager) {
    std::vector<std::shared_ptr<IPlugin>> adapters;
    for (const auto& info : manager.plugins()) {
        adapters.push_back(std::make_shared<ExternalPluginAdapter>(manager, info.id, info.version));
    }
    return adapters;
}

} // namespace agent

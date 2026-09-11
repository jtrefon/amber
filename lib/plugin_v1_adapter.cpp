#include "agent/plugin_v1_adapter.h"

#include <utility>

namespace agent {

V1PluginAdapter::V1PluginAdapter(PluginManager& manager, std::string plugin_id, std::string version)
    : manager_(&manager), id_(std::move(plugin_id)), version_(std::move(version)) {}

bool V1PluginAdapter::initialize(const PluginContext& ctx) {
    // The v1 manager owns the process lifecycle and registers the plugin's
    // tools. The context hands out a const registry (plugins cannot write to
    // the harness directly), so the manager's own entry point takes the
    // registry the host owns - remembered here for the matching disable.
    tools_ = const_cast<ToolRegistry*>(&ctx.tools);
    enabled_ = manager_->enable(id_, *tools_);
    return enabled_;
}

void V1PluginAdapter::shutdown() {
    if (!enabled_ || !tools_)
        return;
    manager_->disable(id_, *tools_);
    enabled_ = false;
}

std::vector<std::shared_ptr<IPlugin>> make_v1_plugin_adapters(PluginManager& manager) {
    std::vector<std::shared_ptr<IPlugin>> adapters;
    for (const auto& info : manager.plugins()) {
        adapters.push_back(std::make_shared<V1PluginAdapter>(manager, info.id, info.version));
    }
    return adapters;
}

} // namespace agent

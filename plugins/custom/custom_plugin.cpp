#include "custom_plugin.h"

#include "agent/extensions.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> CustomPlugin::capabilities() {
    ProviderCapability::Preset preset;
    preset.name = "custom";
    // No endpoint and no key: the dedicated config file supplies both. The
    // preset exists so the provider is listed and selectable, and so an
    // unconfigured one reports exactly what is missing.
    preset.api_base.clear();
    preset.default_model.clear();
    preset.requires_key = false;

    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(
        std::make_unique<ProviderCapability>("openai", std::function<std::unique_ptr<Dialect>()>{},
                                             std::vector<ProviderCapability::Preset>{preset}));
    return caps;
}

} // namespace agent::plugins

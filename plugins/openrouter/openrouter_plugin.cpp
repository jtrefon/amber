#include "openrouter_plugin.h"

#include "agent/extensions.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> OpenRouterPlugin::capabilities() {
    ProviderCapability::Preset preset;
    preset.name = "openrouter";
    preset.api_base = "https://openrouter.ai/api/v1";
    preset.default_model = "openai/gpt-4o";
    preset.requires_key = true;

    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ProviderCapability>(
        "openai", std::function<std::unique_ptr<Dialect>()>{},
        std::vector<ProviderCapability::Preset>{preset}));
    return caps;
}

} // namespace agent::plugins

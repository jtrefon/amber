#include "anthropic_plugin.h"

#include "agent/dialect_anthropic.h"
#include "agent/extensions.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> AnthropicPlugin::capabilities() {
    ProviderCapability::Preset preset;
    preset.name = "anthropic";
    preset.api_base = "https://api.anthropic.com";
    preset.default_model = "claude-sonnet-4-5";
    preset.requires_key = true;

    std::vector<std::unique_ptr<Capability>> caps;
    // This plugin owns the Messages API protocol: switching it off makes the
    // flavor unavailable rather than silently falling back to another one.
    caps.push_back(std::make_unique<ProviderCapability>(
        "anthropic", [] { return make_anthropic_dialect(); },
        std::vector<ProviderCapability::Preset>{preset}));
    return caps;
}

} // namespace agent::plugins

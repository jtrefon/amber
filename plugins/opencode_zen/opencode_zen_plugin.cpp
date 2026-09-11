#include "opencode_zen_plugin.h"

#include "agent/extensions.h"

namespace agent::plugins {

namespace {

constexpr const char* kApiBase = "https://opencode.ai/zen/v1";
constexpr const char* kDefaultModel = "glm-5.2";

} // namespace

std::vector<std::unique_ptr<Capability>> OpencodeZenPlugin::capabilities() {
    ProviderCapability::Preset preset;
    preset.name = "opencode_zen";
    preset.api_base = kApiBase;
    preset.default_model = kDefaultModel;
    preset.requires_key = true;

    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(
        std::make_unique<ProviderCapability>("openai", std::function<std::unique_ptr<Dialect>()>{},
                                             std::vector<ProviderCapability::Preset>{preset}));
    return caps;
}

} // namespace agent::plugins

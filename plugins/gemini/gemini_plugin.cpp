#include "gemini_plugin.h"

#include "agent/dialect_gemini.h"
#include "agent/extensions.h"

#include <memory>

namespace agent::plugins {

bool GeminiPlugin::initialize(const PluginContext&) {
    return true;
}

void GeminiPlugin::shutdown() {
    // Contributions are removed by the runtime's ledger; nothing to release.
}

std::vector<std::unique_ptr<Capability>> GeminiPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;

    ProviderCapability::Preset preset;
    preset.name = "gemini";
    preset.api_base = "https://generativelanguage.googleapis.com";
    preset.default_model = "gemini-2.5-pro";
    preset.requires_key = true;

    std::vector<ProviderCapability::Preset> presets{preset};
    caps.push_back(std::make_unique<ProviderCapability>(
        "gemini", [] { return make_gemini_dialect(); }, presets));
    return caps;
}

} // namespace agent::plugins

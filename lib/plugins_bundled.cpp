#include "agent/plugins_bundled.h"

#include "plugins/anthropic/anthropic_plugin.h"
#include "plugins/gemini/gemini_plugin.h"
#include "plugins/kilocode/kilocode_plugin.h"
#include "plugins/metrics/metrics_plugin.h"
#include "plugins/openrouter/openrouter_plugin.h"

namespace agent {

std::vector<std::shared_ptr<IPlugin>> make_bundled_plugins() {
    std::vector<std::shared_ptr<IPlugin>> plugins;
    // Observability
    plugins.push_back(std::make_shared<plugins::MetricsPlugin>());
    // Providers: the vendor set ships as plugins, so each can be switched off
    // and each owns its own presets, protocol, and provider-specific features.
    plugins.push_back(std::make_shared<plugins::OpenRouterPlugin>());
    plugins.push_back(std::make_shared<plugins::KilocodePlugin>());
    plugins.push_back(std::make_shared<plugins::AnthropicPlugin>());
    plugins.push_back(std::make_shared<plugins::GeminiPlugin>());
    return plugins;
}

} // namespace agent

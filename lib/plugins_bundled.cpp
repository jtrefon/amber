#include "agent/plugins_bundled.h"

#include "plugins/anthropic/anthropic_plugin.h"
#include "plugins/commandcode/commandcode_plugin.h"
#include "plugins/core_tools/core_tools_plugin.h"
#include "plugins/custom/custom_plugin.h"
#include "plugins/deepseek/deepseek_plugin.h"
#include "plugins/gemini/gemini_plugin.h"
#include "plugins/kilocode/kilocode_plugin.h"
#include "plugins/metrics/metrics_plugin.h"
#include "plugins/opencode_go/opencode_go_plugin.h"
#include "plugins/opencode_zen/opencode_zen_plugin.h"
#include "plugins/openrouter/openrouter_plugin.h"

namespace agent {

std::vector<std::shared_ptr<IPlugin>> make_bundled_plugins() {
    std::vector<std::shared_ptr<IPlugin>> plugins;
    // Observability
    plugins.push_back(std::make_shared<plugins::MetricsPlugin>());
    // Tools: the built-in tool set arrives through the same capability path an
    // extension would use, so the path cannot rot. It needs host services
    // (attach_host_services) to construct them.
    plugins.push_back(std::make_shared<plugins::CoreToolsPlugin>());
    // Providers: every provider amber ships is a plugin, so each can be
    // switched off and each owns its presets, protocol, and provider-specific
    // features. The core declares none.
    plugins.push_back(std::make_shared<plugins::CustomPlugin>());
    plugins.push_back(std::make_shared<plugins::OpenRouterPlugin>());
    plugins.push_back(std::make_shared<plugins::KilocodePlugin>());
    plugins.push_back(std::make_shared<plugins::AnthropicPlugin>());
    plugins.push_back(std::make_shared<plugins::GeminiPlugin>());
    plugins.push_back(std::make_shared<plugins::OpencodeGoPlugin>());
    plugins.push_back(std::make_shared<plugins::OpencodeZenPlugin>());
    plugins.push_back(std::make_shared<plugins::CommandcodePlugin>());
    plugins.push_back(std::make_shared<plugins::DeepseekPlugin>());
    return plugins;
}

} // namespace agent

#include "agent/plugins_bundled.h"

#include "plugins/anthropic/anthropic_plugin.h"
#include "plugins/commandcode/commandcode_plugin.h"
#include "plugins/custom/custom_plugin.h"
#include "plugins/deepseek/deepseek_plugin.h"
#include "plugins/gemini/gemini_plugin.h"
#include "plugins/kilocode/kilocode_plugin.h"
#include "plugins/metrics/metrics_plugin.h"
#include "plugins/opencode_go/opencode_go_plugin.h"
#include "plugins/opencode_zen/opencode_zen_plugin.h"
#include "plugins/openrouter/openrouter_plugin.h"
#include "plugins/tool_bash/tool_bash_plugin.h"
#include "plugins/tool_plan/tool_plan_plugin.h"
#include "plugins/tool_process/tool_process_plugin.h"
#include "plugins/tool_read/tool_read_plugin.h"
#include "plugins/tool_search/tool_search_plugin.h"
#include "plugins/tool_task/tool_task_plugin.h"
#include "plugins/tool_write/tool_write_plugin.h"

namespace agent {

std::vector<std::shared_ptr<IPlugin>> make_bundled_plugins() {
    std::vector<std::shared_ptr<IPlugin>> plugins;
    // Observability
    plugins.push_back(std::make_shared<plugins::MetricsPlugin>());
    // Tools: one plugin per tunable unit, so a user can switch off exactly the
    // tool they want to experiment with - "search off" is the pilot the tools
    // spec names. Each carries its own metadata (category, description) and its
    // own documentation, and each arrives through the same capability path an
    // extension would use, so the path cannot rot. They need host services
    // (attach_host_services) to construct their tools.
    plugins.push_back(std::make_shared<plugins::ToolSearchPlugin>());
    plugins.push_back(std::make_shared<plugins::ToolReadPlugin>());
    plugins.push_back(std::make_shared<plugins::ToolWritePlugin>());
    plugins.push_back(std::make_shared<plugins::ToolBashPlugin>());
    plugins.push_back(std::make_shared<plugins::ToolProcessPlugin>());
    plugins.push_back(std::make_shared<plugins::ToolPlanPlugin>());
    plugins.push_back(std::make_shared<plugins::ToolTaskPlugin>());
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

#include "agent/plugins_bundled.h"

#include "plugins/gemini/gemini_plugin.h"
#include "plugins/metrics/metrics_plugin.h"

namespace agent {

std::vector<std::shared_ptr<IPlugin>> make_bundled_plugins() {
    std::vector<std::shared_ptr<IPlugin>> plugins;
    plugins.push_back(std::make_shared<plugins::MetricsPlugin>());
    plugins.push_back(std::make_shared<plugins::GeminiPlugin>());
    return plugins;
}

} // namespace agent

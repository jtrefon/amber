#include "agent/plugins_bundled.h"

#include "plugins/metrics/metrics_plugin.h"

namespace agent {

std::vector<std::shared_ptr<IPlugin>> make_bundled_plugins() {
    std::vector<std::shared_ptr<IPlugin>> plugins;
    plugins.push_back(std::make_shared<plugins::MetricsPlugin>());
    return plugins;
}

} // namespace agent

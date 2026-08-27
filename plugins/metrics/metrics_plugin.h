
#ifndef AGENT_PLUGINS_METRICS_H
#define AGENT_PLUGINS_METRICS_H

#include "agent/plugin_v2.h"
#include <atomic>
#include <chrono>

namespace agent::plugins {

class MetricsPlugin : public IPlugin {
public:
    std::string id() const override { return "metrics"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Metrics"; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override;

    std::vector<Capability> capabilities() const override;

    struct Stats {
        int turns = 0;
        int tool_calls = 0;
        long total_ms = 0;
    };

    const Stats& stats() const { return stats_; }

private:
    void on_turn_start(const Event&);
    void on_turn_end(const Event&);
    void on_tool_before(const Event&);
    void on_tool_after(const Event&);

    Stats stats_;
    std::chrono::steady_clock::time_point turn_start_;
    size_t turn_sub_ = 0;
    size_t tool_before_sub_ = 0;
    size_t tool_after_sub_ = 0;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_METRICS_H

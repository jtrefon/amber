
#ifndef AGENT_PLUGINS_METRICS_H
#define AGENT_PLUGINS_METRICS_H

#include "agent/plugin_core.h"
#include <atomic>
#include <chrono>

namespace agent::plugins {

class MetricsPlugin : public IPlugin {
public:
    std::string id() const override { return "metrics"; }
    std::string version() const override { return "0.4.0"; }
    std::string name() const override { return "Metrics"; }
    std::string description() const override {
        return "Counts turns, tool calls and turn duration.";
    }
    std::string category() const override { return plugin_category::kObservability; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override;

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

    EventBus* bus_ = nullptr; // where the subscriptions live
    Stats stats_;
    std::chrono::steady_clock::time_point turn_start_;
    // Direct bus subscriptions are the plugin's to release: the ledger tracks
    // *capabilities*, so a plugin that observes rather than contributes must
    // unsubscribe in shutdown() or it leaves callbacks behind when disabled.
    size_t turn_sub_ = 0;
    size_t turn_end_sub_ = 0;
    size_t tool_before_sub_ = 0;
    size_t tool_after_sub_ = 0;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_METRICS_H

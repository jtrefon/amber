
#include "metrics_plugin.h"

namespace agent::plugins {

bool MetricsPlugin::initialize(const PluginContext& ctx) {
    turn_sub_ = ctx.event_bus.subscribe(
        EventType::AgentTurnStart,
        [this](const Event& e) { on_turn_start(e); });

    auto end_sub = ctx.event_bus.subscribe(
        EventType::AgentTurnEnd,
        [this](const Event& e) { on_turn_end(e); });

    tool_before_sub_ = ctx.event_bus.subscribe(
        EventType::ToolCallBefore,
        [this](const Event& e) { on_tool_before(e); });

    tool_after_sub_ = ctx.event_bus.subscribe(
        EventType::ToolCallAfter,
        [this](const Event& e) { on_tool_after(e); });

    return true;
}

void MetricsPlugin::shutdown() {
    stats_ = Stats{};
}

std::vector<Capability> MetricsPlugin::capabilities() const {
    return {};
}

void MetricsPlugin::on_turn_start(const Event&) {
    ++stats_.turns;
    turn_start_ = std::chrono::steady_clock::now();
}

void MetricsPlugin::on_turn_end(const Event&) {
    auto now = std::chrono::steady_clock::now();
    stats_.total_ms += std::chrono::duration_cast<std::chrono::milliseconds>(
        now - turn_start_).count();
}

void MetricsPlugin::on_tool_before(const Event&) {
    ++stats_.tool_calls;
}

void MetricsPlugin::on_tool_after(const Event&) {}

} // namespace agent::plugins


#include "plugins/metrics/metrics_plugin.h"
#include "agent/event_bus.h"
#include "test_util.h"
#include <memory>

using namespace agent;
using namespace agent::plugins;

TEST(metrics_plugin_identity) {
    MetricsPlugin p;
    ASSERT_EQ(p.id(), "metrics");
    ASSERT_EQ(p.version(), "1.0.0");
    ASSERT_EQ(p.name(), "Metrics");
}

TEST(metrics_plugin_initialize_subscribes) {
    EventBus bus;
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginContext ctx{bus, tools, cfg, ws};

    MetricsPlugin p;
    ASSERT_TRUE(p.initialize(ctx));

    Event start_ev{EventType::AgentTurnStart, nullptr};
    bus.fire(EventType::AgentTurnStart, start_ev);
    ASSERT_EQ(p.stats().turns, 1);

    Event tool_ev{EventType::ToolCallBefore, nullptr};
    bus.fire(EventType::ToolCallBefore, tool_ev);
    ASSERT_EQ(p.stats().tool_calls, 1);

    Event end_ev{EventType::AgentTurnEnd, nullptr};
    bus.fire(EventType::AgentTurnEnd, end_ev);
    ASSERT(p.stats().total_ms >= 0);
}

TEST(metrics_plugin_tracks_multiple_turns) {
    EventBus bus;
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginContext ctx{bus, tools, cfg, ws};

    MetricsPlugin p;
    p.initialize(ctx);

    for (int i = 0; i < 5; ++i) {
        Event start{EventType::AgentTurnStart, nullptr};
        bus.fire(EventType::AgentTurnStart, start);
        Event end{EventType::AgentTurnEnd, nullptr};
        bus.fire(EventType::AgentTurnEnd, end);
    }
    ASSERT_EQ(p.stats().turns, 5);
}

TEST(metrics_plugin_shutdown_resets) {
    EventBus bus;
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginContext ctx{bus, tools, cfg, ws};

    MetricsPlugin p;
    p.initialize(ctx);

    Event start{EventType::AgentTurnStart, nullptr};
    bus.fire(EventType::AgentTurnStart, start);
    ASSERT_EQ(p.stats().turns, 1);

    p.shutdown();
    ASSERT_EQ(p.stats().turns, 0);
}

TEST(metrics_plugin_capabilities_empty) {
    MetricsPlugin p;
    ASSERT_TRUE(p.capabilities().empty());
}

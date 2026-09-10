// PF-1 framework tests: typed events (spec §6) and the capability ledger
// (spec §3). Hermetic — no host, no network.

#include "agent/events.h"
#include "agent/plugin_capability.h"
#include "test_util.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace agent;

// ---------------------------------------------------------------------------
// PF-1.1 — typed events
// ---------------------------------------------------------------------------

TEST(typed_events_deliver_typed_payload) {
    EventBus bus;
    Events events(bus);
    std::string seen;
    auto sub = events.subscribe<TurnStartedEvent>(
        [&](const TurnStartedEvent& e) { seen = e.prompt; });

    TurnStartedEvent ev;
    ev.prompt = "hello";
    events.publish(ev);
    ASSERT_EQ(seen, std::string("hello"));
}

TEST(typed_events_are_type_isolated) {
    EventBus bus;
    Events events(bus);
    int turns = 0, tools = 0;
    auto a = events.subscribe<TurnStartedEvent>(
        [&](const TurnStartedEvent&) { ++turns; });
    auto b = events.subscribe<ToolRequestedEvent>(
        [&](const ToolRequestedEvent&) { ++tools; });

    TurnStartedEvent ev;
    ev.prompt = "x";
    events.publish(ev);
    ASSERT_EQ(turns, 1);
    ASSERT_EQ(tools, 0);
}

TEST(typed_event_interceptor_can_cancel) {
    EventBus bus;
    Events events(bus);
    bool observed = false;
    auto guard = events.intercept<ToolRequestedEvent>(
        [](ToolRequestedEvent& e) {
            e.cancel = true;
            return false;
        });
    auto obs = events.subscribe<ToolRequestedEvent>(
        [&](const ToolRequestedEvent&) { observed = true; });

    ToolRequestedEvent ev;
    ev.name = "bash";
    bool continued = events.publish(ev);
    ASSERT_FALSE(continued);
    ASSERT_FALSE(observed);
    ASSERT_TRUE(ev.cancel);
}

TEST(typed_event_interceptor_can_modify_payload) {
    EventBus bus;
    Events events(bus);
    auto guard = events.intercept<TurnStartedEvent>(
        [](TurnStartedEvent& e) { e.prompt += " [enhanced]"; return true; });

    TurnStartedEvent ev;
    ev.prompt = "base";
    events.publish(ev);
    ASSERT_EQ(ev.prompt, std::string("base [enhanced]"));
}

TEST(typed_subscription_removes_on_destroy) {
    EventBus bus;
    Events events(bus);
    int count = 0;
    {
        auto sub = events.subscribe<TurnStartedEvent>(
            [&](const TurnStartedEvent&) { ++count; });
        TurnStartedEvent ev;
        ev.prompt = "a";
        events.publish(ev);
    }
    TurnStartedEvent ev2;
    ev2.prompt = "b";
    events.publish(ev2);
    ASSERT_EQ(count, 1);
    ASSERT_FALSE(bus.has_subscribers(EventType::AgentTurnStart));
}

TEST(typed_subscription_can_be_released_early) {
    EventBus bus;
    Events events(bus);
    int count = 0;
    auto sub = events.subscribe<TurnEndedEvent>(
        [&](const TurnEndedEvent&) { ++count; });
    sub.release();
    TurnEndedEvent ev;
    events.publish(ev);
    ASSERT_EQ(count, 0);
    ASSERT_FALSE(bus.has_subscribers(EventType::AgentTurnEnd));
}

TEST(publish_without_subscribers_is_a_noop) {
    EventBus bus;
    Events events(bus);
    ASSERT_FALSE(bus.has_subscribers(EventType::AgentTurnEnd));
    TurnEndedEvent ev;
    events.publish(ev);
    ASSERT_FALSE(bus.has_subscribers(EventType::AgentTurnEnd));
}

TEST(event_bus_tracks_subscriber_counts) {
    EventBus bus;
    ASSERT_FALSE(bus.has_subscribers(EventType::AgentTurnEnd));

    size_t observer = bus.subscribe(EventType::AgentTurnEnd, [](const Event&) {});
    ASSERT_TRUE(bus.has_subscribers(EventType::AgentTurnEnd));

    size_t interceptor = bus.intercept(EventType::AgentTurnEnd,
                                       [](Event&) { return true; });
    bus.unsubscribe(observer);
    ASSERT_TRUE(bus.has_subscribers(EventType::AgentTurnEnd));
    bus.unsubscribe(interceptor);
    ASSERT_FALSE(bus.has_subscribers(EventType::AgentTurnEnd));

    bus.subscribe(EventType::AgentTurnEnd, [](const Event&) {});
    bus.clear();
    ASSERT_FALSE(bus.has_subscribers(EventType::AgentTurnEnd));
}

// ---------------------------------------------------------------------------
// PF-1.3 — capability ledger (spec §3, PLG-01)
// ---------------------------------------------------------------------------

namespace {

Contribution make_contribution(CapabilityKind kind, const std::string& name,
                               std::vector<std::string>& removals) {
    Contribution c;
    c.kind = kind;
    c.name = name;
    c.remove = [&removals, name] { removals.push_back(name); };
    return c;
}

} // namespace

TEST(ledger_unwinds_in_reverse_order) {
    PluginLedger ledger;
    std::vector<std::string> removed;
    ledger.record("plug", make_contribution(CapabilityKind::Tool, "a", removed));
    ledger.record("plug", make_contribution(CapabilityKind::Command, "b", removed));
    ledger.record("plug", make_contribution(CapabilityKind::PromptBlock, "c", removed));

    ASSERT_EQ(ledger.size("plug"), 3u);
    ledger.unwind("plug");

    ASSERT_EQ(removed.size(), 3u);
    ASSERT_EQ(removed[0], std::string("c"));
    ASSERT_EQ(removed[1], std::string("b"));
    ASSERT_EQ(removed[2], std::string("a"));
    ASSERT_EQ(ledger.size("plug"), 0u);
}

TEST(ledger_unwind_is_per_plugin) {
    PluginLedger ledger;
    std::vector<std::string> removals;
    ledger.record("alpha", make_contribution(CapabilityKind::Tool, "x", removals));
    ledger.record("beta", make_contribution(CapabilityKind::Tool, "y", removals));

    ledger.unwind("alpha");
    ASSERT_EQ(removals.size(), 1u);
    ASSERT_EQ(removals[0], std::string("x"));
    ASSERT_EQ(ledger.size("beta"), 1u);

    ledger.unwind("beta");
    ASSERT_EQ(removals.size(), 2u);
    ASSERT_EQ(ledger.size("alpha"), 0u);
}

TEST(ledger_disable_removes_every_contribution) {
    std::vector<std::string> tools{"core.read", "core.write"};
    PluginLedger ledger;
    Contribution greet;
    greet.kind = CapabilityKind::Tool;
    greet.name = "greet";
    greet.remove = [&tools] {
        tools.erase(std::remove(tools.begin(), tools.end(), std::string("greet")),
                    tools.end());
    };
    ledger.record("plug", greet);
    tools.push_back("greet");

    ASSERT_EQ(tools.size(), 3u);
    ledger.unwind("plug");
    ASSERT_EQ(tools.size(), 2u);
    ASSERT_EQ(tools[0], std::string("core.read"));
    ASSERT_EQ(tools[1], std::string("core.write"));
}

TEST(ledger_unwind_unknown_plugin_is_noop) {
    PluginLedger ledger;
    ledger.unwind("nobody");
    ASSERT_EQ(ledger.size("nobody"), 0u);
}

TEST(ledger_ignores_empty_removal_handles) {
    PluginLedger ledger;
    Contribution hollow;
    hollow.kind = CapabilityKind::Setting;
    hollow.name = "no-op";
    ledger.record("plug", hollow);
    ledger.unwind("plug");
    ASSERT_EQ(ledger.size("plug"), 0u);
}

TEST(ledger_reports_owners_and_contributions) {
    PluginLedger ledger;
    std::vector<std::string> removals;
    ledger.record("alpha", make_contribution(CapabilityKind::Tool, "a", removals));
    ledger.record("beta", make_contribution(CapabilityKind::Command, "b", removals));

    std::vector<std::string> owners = ledger.owners();
    std::sort(owners.begin(), owners.end());
    ASSERT_EQ(owners.size(), 2u);
    ASSERT_EQ(owners[0], std::string("alpha"));
    ASSERT_EQ(owners[1], std::string("beta"));

    auto alpha = ledger.contributions("alpha");
    ASSERT_EQ(alpha.size(), 1u);
    ASSERT_EQ(alpha[0].name, std::string("a"));
    ASSERT_TRUE(alpha[0].kind == CapabilityKind::Tool);
}

TEST(ledger_clear_unwinds_everything) {
    PluginLedger ledger;
    std::vector<std::string> removals;
    ledger.record("alpha", make_contribution(CapabilityKind::Tool, "a", removals));
    ledger.record("beta", make_contribution(CapabilityKind::Tool, "b", removals));

    ledger.clear();
    ASSERT_EQ(removals.size(), 2u);
    ASSERT_TRUE(ledger.owners().empty());
}

// ---------------------------------------------------------------------------
// PF-1.3 — capability protocol
// ---------------------------------------------------------------------------

namespace {

class StubCapability : public Capability {
public:
    std::string name() const override { return "greet"; }
    CapabilityKind kind() const override { return CapabilityKind::Tool; }
    InstallResult install(PluginServices&) override {
        InstallResult r;
        r.ok = true;
        r.contribution.kind = CapabilityKind::Tool;
        r.contribution.name = "greet";
        return r;
    }
};

class FailingCapability : public Capability {
public:
    std::string name() const override { return "broken"; }
    CapabilityKind kind() const override { return CapabilityKind::Command; }
    InstallResult install(PluginServices&) override {
        InstallResult r;
        r.ok = false;
        r.error = "registry refused the contribution";
        return r;
    }
};

} // namespace

TEST(capability_reports_kind_and_name) {
    StubCapability cap;
    ASSERT_EQ(cap.name(), std::string("greet"));
    ASSERT_TRUE(cap.kind() == CapabilityKind::Tool);
}

TEST(capability_install_reports_the_contribution) {
    StubCapability cap;
    PluginServices services;
    InstallResult r = cap.install(services);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.contribution.name, std::string("greet"));
}

TEST(capability_install_can_fail_with_a_reason) {
    FailingCapability cap;
    PluginServices services;
    InstallResult r = cap.install(services);
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.error.empty());
}

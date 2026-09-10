// PF-1 framework tests: typed events (spec §6) and the capability ledger
// (spec §3). Hermetic — no host, no network.

#include "agent/events.h"
#include "agent/extensions.h"
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

namespace {

// A tool with no behaviour: enough to prove registry ownership and removal.
class SilentTool : public Tool {
public:
    explicit SilentTool(std::string name) : name_(std::move(name)) {}
    std::string name() const noexcept override { return name_; }
    std::string description() const noexcept override { return "test tool"; }
    json parameters_schema() const override { return json::object(); }
    ToolResult execute(const json&) const override { return {true, "", "", json{}}; }

private:
    std::string name_;
};

// The smallest harness a capability can install into.
struct TestHarness {
    ToolRegistry tools;
    PromptRegistry prompts;
    CommandRegistry commands;
    PluginSettingsStore settings;
    EventBus bus;
    PluginServices services{tools, prompts, commands, settings, bus};
};

} // namespace

TEST(capability_install_reports_the_contribution) {
    StubCapability cap;
    TestHarness h;
    InstallResult r = cap.install(h.services);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.contribution.name, std::string("greet"));
}

TEST(capability_install_can_fail_with_a_reason) {
    FailingCapability cap;
    TestHarness h;
    InstallResult r = cap.install(h.services);
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.error.empty());
}

// ---------------------------------------------------------------------------
// PF-1.4 — contribution registries
// ---------------------------------------------------------------------------

TEST(prompt_registry_orders_by_priority_then_registration) {
    PromptRegistry prompts;
    auto a = prompts.add("plug", "late", 300, [] { return std::string("C"); });
    auto b = prompts.add("plug", "early", 100, [] { return std::string("A"); });
    auto c = prompts.add("plug", "mid", 200, [] { return std::string("B"); });

    auto rendered = prompts.render_all();
    ASSERT_EQ(rendered.size(), 3u);
    ASSERT_EQ(rendered[0], std::string("A"));
    ASSERT_EQ(rendered[1], std::string("B"));
    ASSERT_EQ(rendered[2], std::string("C"));
}

TEST(prompt_registry_skips_empty_blocks) {
    PromptRegistry prompts;
    prompts.add("plug", "silent", 100, [] { return std::string(); });
    prompts.add("plug", "loud", 200, [] { return std::string("here"); });
    auto rendered = prompts.render_all();
    ASSERT_EQ(rendered.size(), 1u);
    ASSERT_EQ(rendered[0], std::string("here"));
}

TEST(prompt_registry_removal_takes_only_that_block) {
    PromptRegistry prompts;
    auto keep = prompts.add("plug", "keep", 100, [] { return std::string("keep"); });
    auto drop = prompts.add("plug", "drop", 200, [] { return std::string("drop"); });
    ASSERT_EQ(prompts.size(), 2u);

    drop.remove();
    ASSERT_EQ(prompts.size(), 1u);
    auto rendered = prompts.render_all();
    ASSERT_EQ(rendered.size(), 1u);
    ASSERT_EQ(rendered[0], std::string("keep"));

    keep.remove();
    ASSERT_EQ(prompts.size(), 0u);
}

TEST(command_registry_dispatches_registered_leaf) {
    CommandRegistry commands;
    std::string seen;
    CommandRegistry::Handler handler = [&](const std::string& arg) { seen = arg; };
    auto contribution = commands.add("plug", "hello", R"({"greet":{"help":"x"}})",
                                     {{"greet", handler}});

    ASSERT(commands.dispatch("hello", "greet", "world"));
    ASSERT_EQ(seen, std::string("world"));
    // Unknown paths and unknown roots are reported, never silently ignored.
    ASSERT_FALSE(commands.dispatch("hello", "nope", ""));
    ASSERT_FALSE(commands.dispatch("other", "greet", ""));

    contribution.remove();
    ASSERT_FALSE(commands.dispatch("hello", "greet", ""));
    ASSERT_EQ(commands.size(), 0u);
}

TEST(settings_store_round_trips_per_owner) {
    PluginSettingsStore settings;
    settings.set("alpha", "level", "3");
    settings.set("beta", "level", "9");

    ASSERT_EQ(settings.get("alpha", "level"), std::string("3"));
    ASSERT_EQ(settings.get("beta", "level"), std::string("9"));
    ASSERT_EQ(settings.get("alpha", "missing"), std::string(""));
    ASSERT_EQ(settings.get("nobody", "level"), std::string(""));
    ASSERT_TRUE(settings.has("alpha"));
    ASSERT_FALSE(settings.has("nobody"));
}

TEST(settings_store_can_declare_unset_keys) {
    PluginSettingsStore settings;
    settings.declare("alpha", "endpoint", "where to connect");
    auto items = settings.items();
    ASSERT_EQ(items.size(), 1u);
    ASSERT_EQ(items[0].owner, std::string("alpha"));
    ASSERT_EQ(items[0].name, std::string("endpoint"));
    ASSERT_EQ(items[0].detail, std::string("where to connect"));

    settings.undeclare("alpha", "endpoint");
    ASSERT_TRUE(settings.items().empty());
}

TEST(tool_capability_registers_and_removes_exactly_its_tool) {
    TestHarness h;
    h.tools.register_tool(std::make_unique<SilentTool>("host.tool"));

    ToolCapability cap("greet", std::make_unique<SilentTool>("plugin.greet"));
    h.services.set_owner("plug");
    InstallResult r = cap.install(h.services);
    ASSERT_TRUE(r.ok);
    ASSERT((bool)h.tools.find("plugin.greet"));
    ASSERT((bool)h.tools.find("host.tool"));

    r.contribution.remove();
    ASSERT_FALSE((bool)h.tools.find("plugin.greet"));
    ASSERT((bool)h.tools.find("host.tool"));
}

TEST(command_capability_tags_its_owner) {
    TestHarness h;
    h.services.set_owner("plug");
    CommandCapability cap("hello", "{}", {});
    InstallResult r = cap.install(h.services);
    ASSERT_TRUE(r.ok);

    auto items = h.commands.items();
    ASSERT_EQ(items.size(), 1u);
    ASSERT_EQ(items[0].owner, std::string("plug"));
    ASSERT(r.contribution.remove != nullptr);
}

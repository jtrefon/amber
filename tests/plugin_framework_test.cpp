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
    auto sub =
        events.subscribe<TurnStartedEvent>([&](const TurnStartedEvent& e) { seen = e.prompt; });

    TurnStartedEvent ev;
    ev.prompt = "hello";
    events.publish(ev);
    ASSERT_EQ(seen, std::string("hello"));
}

TEST(typed_events_are_type_isolated) {
    EventBus bus;
    Events events(bus);
    int turns = 0, tools = 0;
    auto a = events.subscribe<TurnStartedEvent>([&](const TurnStartedEvent&) { ++turns; });
    auto b = events.subscribe<ToolRequestedEvent>([&](const ToolRequestedEvent&) { ++tools; });

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
    auto guard = events.intercept<ToolRequestedEvent>([](ToolRequestedEvent& e) {
        e.cancel = true;
        return false;
    });
    auto obs =
        events.subscribe<ToolRequestedEvent>([&](const ToolRequestedEvent&) { observed = true; });

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
    auto guard = events.intercept<TurnStartedEvent>([](TurnStartedEvent& e) {
        e.prompt += " [enhanced]";
        return true;
    });

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
        auto sub = events.subscribe<TurnStartedEvent>([&](const TurnStartedEvent&) { ++count; });
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
    auto sub = events.subscribe<TurnEndedEvent>([&](const TurnEndedEvent&) { ++count; });
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

    size_t interceptor = bus.intercept(EventType::AgentTurnEnd, [](Event&) { return true; });
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
    ledger.record("plug", make_contribution(CapabilityKind::Panel, "b", removed));
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
        tools.erase(std::remove(tools.begin(), tools.end(), std::string("greet")), tools.end());
    };
    ledger.record("plug", greet);
    tools.emplace_back("greet");

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
    hollow.kind = CapabilityKind::Wallet;
    hollow.name = "no-op";
    ledger.record("plug", hollow);
    ledger.unwind("plug");
    ASSERT_EQ(ledger.size("plug"), 0u);
}

TEST(ledger_reports_owners_and_contributions) {
    PluginLedger ledger;
    std::vector<std::string> removals;
    ledger.record("alpha", make_contribution(CapabilityKind::Tool, "a", removals));
    ledger.record("beta", make_contribution(CapabilityKind::Panel, "b", removals));

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
    CapabilityKind kind() const override { return CapabilityKind::Panel; }
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
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    AllowanceRegistry allowances;
    EventBus bus;
    CommandRegistry commands;
    PluginServices services{tools, prompts, status, panels, wallets, allowances, bus, commands};
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

// Placement is a property of the block, and rendering one region never returns
// another's text. Tool documentation declares System so it stays in the stable
// prefix; a block that changes per turn belongs in the tail.
TEST(prompt_registry_partitions_blocks_by_placement) {
    PromptRegistry prompts;
    prompts.add("plug", "doc", 100, [] { return std::string("DOC"); }, PromptPlacement::System);
    prompts.add("plug", "mem", 100, [] { return std::string("MEM"); }, PromptPlacement::Head);
    prompts.add("plug", "tail", 100, [] { return std::string("TAIL"); });
    prompts.add("plug", "doc2", 200, [] { return std::string("DOC2"); }, PromptPlacement::System);

    auto system = prompts.render_all(PromptPlacement::System);
    ASSERT_EQ(system.size(), 2u);
    ASSERT_EQ(system[0], std::string("DOC"));
    ASSERT_EQ(system[1], std::string("DOC2"));

    auto head = prompts.render_all(PromptPlacement::Head);
    ASSERT_EQ(head.size(), 1u);
    ASSERT_EQ(head[0], std::string("MEM"));

    // Either region with no blocks is empty rather than a default.
    ASSERT_TRUE(prompts.render_all(PromptPlacement::Tail).size() == 1u);
    prompts.render_all(PromptPlacement::Head);
    ASSERT_EQ(prompts.size(), 4u);
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

// A tool capability's factory receives the harness services, because a tool is
// not pure data: the bash tool needs the job service, todowrite needs the todo
// store, task needs the sub-agent executor. That injection is what lets core
// tools become plugin contributions.
TEST(tool_capability_factory_receives_host_services) {
    TestHarness h;
    agent::HostServices host;
    h.services.host = &host;

    bool saw_services = false;
    ToolCapability cap(
        "greet", [&saw_services](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            saw_services = services.host != nullptr;
            std::vector<std::unique_ptr<Tool>> tools;
            tools.push_back(std::make_unique<SilentTool>("plugin.greet"));
            return tools;
        });

    h.services.set_owner("plug");
    InstallResult r = cap.install(h.services);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(saw_services);
    ASSERT((bool)h.tools.find("plugin.greet"));
}

// A factory that declines (returns nothing) is a capability that did not
// install, not a crash: a plugin can gate a tool on configuration.
TEST(tool_capability_factory_may_decline) {
    TestHarness h;
    ToolCapability cap("optional",
                       [](PluginServices&) -> std::vector<std::unique_ptr<Tool>> { return {}; });
    h.services.set_owner("plug");
    InstallResult r = cap.install(h.services);
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.declined);
    ASSERT_EQ(h.tools.snapshot_tools().size(), 0u);
}

// Names may collide. Two plugins can contribute a tool under the same name, and
// the later registration wins — a plugin overriding a host tool is a feature,
// not an accident. What must never happen is one plugin's unwinding reaching
// across owners and taking another plugin's tool with it: the ledger's contract
// is "removes exactly what this plugin added".
TEST(tool_capability_unwind_cannot_remove_another_plugins_tool) {
    TestHarness h;

    ToolCapability alpha("shared", std::make_unique<SilentTool>("shared.tool"));
    h.services.set_owner("alpha");
    InstallResult a = alpha.install(h.services);
    ASSERT_TRUE(a.ok);

    ToolCapability beta("shared", std::make_unique<SilentTool>("shared.tool"));
    h.services.set_owner("beta");
    InstallResult b = beta.install(h.services);
    ASSERT_TRUE(b.ok);

    // beta's instance replaced alpha's under the same name: the registered tool
    // belongs to beta now, and alpha's contribution is already superseded.
    ASSERT_TRUE((bool)h.tools.find("shared.tool"));

    // Unwinding alpha must not remove beta's tool.
    a.contribution.remove();
    ASSERT_TRUE((bool)h.tools.find("shared.tool"));

    // Unwinding beta removes its own.
    b.contribution.remove();
    ASSERT_FALSE((bool)h.tools.find("shared.tool"));
}

// Several tools from one capability go away together: the ledger records a
// single contribution, so its removal must undo all of them. (The process
// tools are the reason this exists.)
TEST(tool_capability_installs_and_removes_a_group) {
    TestHarness h;
    h.tools.register_tool(std::make_unique<SilentTool>("host.tool"));

    ToolCapability cap("process", [](PluginServices&) {
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::make_unique<SilentTool>("process_start"));
        tools.push_back(std::make_unique<SilentTool>("process_read"));
        tools.push_back(std::make_unique<SilentTool>("process_stop"));
        return tools;
    });
    h.services.set_owner("plug");
    InstallResult r = cap.install(h.services);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(h.tools.snapshot_tools().size(), 4u);

    r.contribution.remove();
    ASSERT((bool)h.tools.find("host.tool"));
    ASSERT_FALSE((bool)h.tools.find("process_start"));
    ASSERT_FALSE((bool)h.tools.find("process_read"));
    ASSERT_FALSE((bool)h.tools.find("process_stop"));
}

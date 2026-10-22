// End-to-end for the command contribution path: a plugin's CommandSpec is
// merged into the command tree and its leaves dispatch through the action
// registry, the same two registries the TUI uses, without needing a Tui.

#include "agent/extensions.h"
#include "test_util.h"
#include "tui/action_registry.h"
#include "tui/plugin_commands.h"
#include "tui/setting_registry.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace agent;

namespace {

std::vector<CommandRegistry::Entry> make_entries() {
    CommandRegistry::Entry e;
    e.owner = "hello";
    e.spec.name = "hello";
    e.spec.help = "Say hello";
    e.spec.man = "Usage: /hello greet <name>";
    e.spec.subtree = nlohmann::json::parse(R"({"greet": {"help": "Greet someone"}})");
    e.spec.handlers["greet"] = [](const std::string& arg) {
        return "Hello, " + (arg.empty() ? std::string("world") : arg) + "!";
    };
    return {e};
}

} // namespace

TEST(command_registry_items_describe_the_workspace) {
    CommandRegistry registry;
    CommandSpec spec;
    spec.name = "demo";
    spec.help = "Demo";
    spec.subtree = nlohmann::json::parse(R"({"go": {"help": "go"}})");
    spec.handlers["go"] = [](const std::string&) { return "ok"; };
    const Contribution contribution = registry.add("acme", std::move(spec));
    const auto items = registry.items();
    ASSERT_EQ(items.size(), 1u);
    ASSERT_EQ(items[0].owner, std::string("acme"));
    ASSERT_EQ(items[0].name, std::string("demo"));
    ASSERT(items[0].detail.find("/plugin acme") != std::string::npos);
    ASSERT(items[0].detail.find("1 handler") != std::string::npos);
    contribution.remove();
    ASSERT(registry.items().empty());
}

TEST(plugin_command_binds_leaf_and_dispatches) {
    auto entries = make_entries();
    tui::SettingRegistry settings;
    tui::ActionRegistry actions;
    std::vector<std::string> printed;

    const std::size_t bound = tui::install_plugin_commands(
        [&entries]() -> const std::vector<CommandRegistry::Entry>& { return entries; }, settings,
        actions, [&printed](const std::string& s) { printed.push_back(s); });

    ASSERT_EQ(bound, 1u);

    // The host mounts the contribution under /plugin/<id>; the leaf still
    // carries the derived action.
    const auto& tree = settings.command_tree();
    ASSERT_EQ(tree["commands"]["plugin"]["children"]["hello"]["children"]["greet"]["action"]
                  .get<std::string>(),
              std::string("plugin.hello.greet"));

    // Dispatching the action runs the handler and prints its return value.
    ASSERT_TRUE(actions.dispatch("plugin.hello.greet", "amber"));
    ASSERT_EQ(printed.size(), 1u);
    ASSERT_EQ(printed[0], std::string("Hello, amber!"));
}

TEST(plugin_command_resolves_the_live_handler) {
    // A disable/re-enable replaces the handler; the bound action must use the
    // live one, not a captured copy.
    auto entries = make_entries();
    tui::SettingRegistry settings;
    tui::ActionRegistry actions;
    std::vector<std::string> printed;

    tui::install_plugin_commands(
        [&entries]() -> const std::vector<CommandRegistry::Entry>& { return entries; }, settings,
        actions, [&printed](const std::string& s) { printed.push_back(s); });

    entries[0].spec.handlers["greet"] = [](const std::string&) { return "live"; };
    ASSERT_TRUE(actions.dispatch("plugin.hello.greet", ""));
    ASSERT_EQ(printed.back(), std::string("live"));

    // With the contribution gone, the action reports unavailable.
    entries.clear();
    ASSERT_TRUE(actions.dispatch("plugin.hello.greet", ""));
    ASSERT_EQ(printed.back(), std::string("/plugin hello: command unavailable"));
}

// A plugin cannot mount into a core namespace: a legacy spec.name that names
// one is still placed under the plugin workspace, and the documented core
// fields are untouched.
TEST(plugin_command_cannot_extend_a_documented_root) {
    auto entries = make_entries();
    entries[0].spec.name = "set";
    entries[0].spec.subtree = nlohmann::json::parse(R"({"evil": {"help": "evil"}})");
    entries[0].spec.handlers["evil"] = [](const std::string&) { return "extended"; };

    tui::SettingRegistry settings;
    ASSERT(settings.load_completions_json("completions.json"));
    const std::string help = settings.help_for("set.model");
    const std::string action =
        settings.command_tree()["commands"]["set"]["action"].get<std::string>();
    tui::ActionRegistry actions;
    std::vector<std::string> printed;

    const std::size_t bound = tui::install_plugin_commands(
        [&entries]() -> const std::vector<CommandRegistry::Entry>& { return entries; }, settings,
        actions, [&printed](const std::string& s) { printed.push_back(s); });

    ASSERT_EQ(bound, 1u);
    bool evil = false;
    for (const auto& k : settings.complete("plugin.hello.set"))
        if (k == "plugin.hello.set.evil")
            evil = true;
    ASSERT(evil);
    ASSERT_EQ(settings.help_for("set.model"), help);
    ASSERT_EQ(settings.command_tree()["commands"]["set"]["action"].get<std::string>(), action);
    ASSERT_TRUE(actions.dispatch("plugin.hello.evil", ""));
    ASSERT_EQ(printed.back(), "extended");
}

TEST(plugin_workspace_resolves_with_and_without_legacy_alias) {
    auto entries = make_entries();
    tui::SettingRegistry settings;
    ASSERT(settings.load_completions_json("completions.json"));
    tui::ActionRegistry actions;
    tui::install_plugin_commands(
        [&entries]() -> const std::vector<CommandRegistry::Entry>& { return entries; }, settings,
        actions, [](const std::string&) {});

    auto m = settings.resolve_command({"plugin", "hello", "greet"});
    ASSERT(m.node);
    ASSERT_EQ(m.consumed, 3u);
    ASSERT_EQ(m.node->at("action").get<std::string>(), std::string("plugin.hello.greet"));

    // One-release compatibility: the old root expands to the workspace path,
    // and only dispatches the arguments after the legacy token.
    m = settings.resolve_command({"hello", "greet", "amber"});
    ASSERT(m.node);
    ASSERT_EQ(m.consumed, 1u);
    ASSERT_EQ(m.node->at("action").get<std::string>(), std::string("plugin.hello.greet"));

    // A removed root whose plugin is not installed is unknown, not a partial
    // match against the /plugin namespace.
    m = settings.resolve_command({"mcp", "list"});
    ASSERT(!m.node);
}

TEST(plugin_command_leaves_without_a_handler_stay_inert) {
    CommandRegistry::Entry e;
    e.owner = "demo";
    e.spec.name = "demo";
    e.spec.subtree = nlohmann::json::parse(R"({"docs": {"help": "documented only"}})");
    // No handler for "docs": documented in the drawer, not executable.
    std::vector<CommandRegistry::Entry> entries{e};

    tui::SettingRegistry settings;
    tui::ActionRegistry actions;
    const std::size_t bound = tui::install_plugin_commands(
        [&entries]() -> const std::vector<CommandRegistry::Entry>& { return entries; }, settings,
        actions, [](const std::string&) {});

    ASSERT_EQ(bound, 0u);
    ASSERT_FALSE(settings.help_for("plugin.demo.docs").empty());
    ASSERT_FALSE(actions.has("plugin.demo.docs"));
}

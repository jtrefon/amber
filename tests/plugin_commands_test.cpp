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
    e.spec.root = "hello";
    e.spec.help = "Say hello";
    e.spec.man = "Usage: /hello greet <name>";
    e.spec.subtree = nlohmann::json::parse(R"({"greet": {"help": "Greet someone"}})");
    e.spec.handlers["greet"] = [](const std::string& arg) {
        return "Hello, " + (arg.empty() ? std::string("world") : arg) + "!";
    };
    return {e};
}

} // namespace

TEST(plugin_command_binds_leaf_and_dispatches) {
    auto entries = make_entries();
    tui::SettingRegistry settings;
    tui::ActionRegistry actions;
    std::vector<std::string> printed;

    const std::size_t bound = tui::install_plugin_commands(
        [&entries]() -> const std::vector<CommandRegistry::Entry>& { return entries; }, settings,
        actions, [&printed](const std::string& s) { printed.push_back(s); });

    ASSERT_EQ(bound, 1u);

    // The tree resolves the leaf and carries the derived action.
    const auto& tree = settings.command_tree();
    ASSERT_EQ(tree["commands"]["hello"]["children"]["greet"]["action"].get<std::string>(),
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
    ASSERT_EQ(printed.back(), std::string("/hello: command unavailable"));
}

TEST(plugin_command_leaves_without_a_handler_stay_inert) {
    CommandRegistry::Entry e;
    e.owner = "demo";
    e.spec.root = "demo";
    e.spec.subtree = nlohmann::json::parse(R"({"docs": {"help": "documented only"}})");
    // No handler for "docs": documented in the drawer, not executable.
    std::vector<CommandRegistry::Entry> entries{e};

    tui::SettingRegistry settings;
    tui::ActionRegistry actions;
    const std::size_t bound = tui::install_plugin_commands(
        [&entries]() -> const std::vector<CommandRegistry::Entry>& { return entries; }, settings,
        actions, [](const std::string&) {});

    ASSERT_EQ(bound, 0u);
    ASSERT_FALSE(settings.help_for("demo.docs").empty());
    ASSERT_FALSE(actions.has("plugin.demo.docs"));
}

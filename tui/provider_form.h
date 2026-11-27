#ifndef AMBER_TUI_PROVIDER_FORM_H
#define AMBER_TUI_PROVIDER_FORM_H

#include <string>
#include <vector>

#include "agent/config.h"
#include "tui/setting_registry.h"

namespace tui {

// Provider edit form (Server URL / API Key / Model). Returns true when the
// user saved, false on cancel. The context window is a deliberate pin, set
// with /set model context, not a provider-form field (a provider default that
// an existing pin outranks was the bug in #290).
//
// Shared by /provider, the settings screen and /profile add: all three edit a
// connection, and a second field list would drift from this one.
bool edit_provider_form(agent::Config& cfg, const std::string& title);

// One per-provider action, derived from the /provider command tree. The
// provider picker renders these rows instead of a C++ verb list; a capability
// that is not in completions.json cannot appear in the menu.
struct ProviderVerb {
    std::string verb;   // "edit", "activate", ...
    std::string label;  // JSON help, the drawer/menu label
    std::string action; // JSON action, dispatched with the provider name
};

// The verbs a provider row offers: every documented /provider child except
// the whole-domain verbs (list, add). Header-only so the command-surface test
// can prove the picker and the tree agree without linking the TUI.
inline std::vector<ProviderVerb> provider_verbs(const SettingRegistry& settings) {
    std::vector<ProviderVerb> out;
    const auto& tree = settings.command_tree();
    if (!tree.contains("commands") || !tree["commands"].is_object())
        return out;
    const auto& commands = tree["commands"];
    if (!commands.contains("provider") || !commands["provider"].is_object())
        return out;
    const auto& node = commands["provider"];
    if (!node.contains("children") || !node["children"].is_object())
        return out;
    for (auto it = node["children"].begin(); it != node["children"].end(); ++it) {
        const std::string& verb = it.key();
        if (verb == "list" || verb == "add")
            continue;
        if (!it.value().is_object() || !it.value().contains("action") ||
            !it.value()["action"].is_string())
            continue;
        ProviderVerb v;
        v.verb = verb;
        v.label = settings.help_for("provider." + verb);
        if (v.label.empty())
            v.label = verb;
        v.action = it.value()["action"].get<std::string>();
        out.push_back(std::move(v));
    }
    return out;
}

} // namespace tui

#endif // AMBER_TUI_PROVIDER_FORM_H

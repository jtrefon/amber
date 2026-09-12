#include "plugin_commands.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace tui {

namespace {

using Entries = std::vector<agent::CommandRegistry::Entry>;

// Walk a command subtree and stamp each executable leaf with its derived action
// (`plugin.<id>.<path>`), binding the action to a dispatch-time lookup. A leaf
// with no handler stays inert. Returns the number of actions bound.
std::size_t bind_leaves(const agent::CommandRegistry::Entry& entry, nlohmann::json& children,
                        const std::string& prefix, ActionRegistry& actions,
                        const std::function<const Entries&()>& entries,
                        const std::function<void(const std::string&)>& output) {
    if (!children.is_object())
        return 0;
    std::size_t bound = 0;
    for (auto it = children.begin(); it != children.end(); ++it) {
        const std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it.value().contains("children") && it.value()["children"].is_object()) {
            bound += bind_leaves(entry, it.value()["children"], path, actions, entries, output);
            continue;
        }
        if (entry.spec.handlers.find(path) == entry.spec.handlers.end())
            continue; // inert leaf: documented, not executable
        const std::string root = entry.spec.root;
        const std::string action = "plugin." + entry.owner + "." + path;
        it.value()["action"] = action;
        actions.register_action(action, [root, path, entries, output](const std::string& arg) {
            for (const auto& live : entries()) {
                if (live.spec.root != root)
                    continue;
                auto h = live.spec.handlers.find(path);
                if (h == live.spec.handlers.end())
                    break;
                std::string out = h->second(arg);
                if (!out.empty())
                    output(out);
                return;
            }
            output("/" + root + ": command unavailable");
        });
        ++bound;
    }
    return bound;
}

} // namespace

std::size_t install_plugin_commands(const std::function<const Entries&()>& entries,
                                    SettingRegistry& settings, ActionRegistry& actions,
                                    const std::function<void(const std::string&)>& output) {
    std::size_t bound = 0;
    for (const auto& entry : entries()) {
        nlohmann::json node = nlohmann::json::object();
        if (!entry.spec.help.empty())
            node["help"] = entry.spec.help;
        if (!entry.spec.man.empty())
            node["man"] = entry.spec.man;
        node["children"] = entry.spec.subtree;

        bound += bind_leaves(entry, node["children"], "", actions, entries, output);

        nlohmann::json merge = nlohmann::json::object();
        merge[entry.spec.root] = std::move(node);
        settings.merge_completions_json(merge);
    }
    return bound;
}

} // namespace tui

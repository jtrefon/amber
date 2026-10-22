#include "plugin_commands.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace tui {

namespace {

using Entries = std::vector<agent::CommandRegistry::Entry>;

// Bind one handler leaf: stamp the node's action and register a closure that
// resolves the handler from the LIVE registry at dispatch time, so a
// disable/re-enable can never dispatch a stale closure.
void bind_handler(const agent::CommandRegistry::Entry& entry, nlohmann::json& node,
                  const std::string& path, ActionRegistry& actions,
                  const std::function<const Entries&()>& entries,
                  const std::function<void(const std::string&)>& output) {
    const std::string owner = entry.owner;
    const std::string name = entry.spec.name;
    // The workspace node binds under its own name (a namespace whose arguments
    // are not child commands); every other leaf uses its dotted path.
    const std::string action =
        path.empty() ? "plugin." + owner + "." + name : "plugin." + owner + "." + path;
    node["action"] = action;
    actions.register_action(action, [owner, name, path, entries, output](const std::string& arg) {
        for (const auto& live : entries()) {
            if (live.owner != owner || live.spec.name != name)
                continue;
            auto h = live.spec.handlers.find(path);
            if (h == live.spec.handlers.end())
                break;
            std::string out = h->second(arg);
            if (!out.empty())
                output(out);
            return;
        }
        const std::string target =
            name == owner ? "/plugin " + owner : "/plugin " + owner + " " + name;
        output(target + ": command unavailable");
    });
}

// Walk a command subtree and stamp each executable leaf with its derived action
// (`plugin.<id>.<path>`). A leaf with no handler stays inert. Returns the
// number of actions bound.
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
        bind_handler(entry, it.value(), path, actions, entries, output);
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

        // A handler keyed by the empty path binds the ROOT node, which is how a
        // namespace that takes free-form arguments (`/prompt <server> <name>`)
        // is executable at all — its arguments are not child commands.
        if (entry.spec.handlers.count("") > 0) {
            bind_handler(entry, node, "", actions, entries, output);
            ++bound;
        }

        bound += bind_leaves(entry, node["children"], "", actions, entries, output);

        // The host owns the mount: a plugin never names a root. A command
        // whose spec.name equals its plugin id mounts at the workspace
        // itself; a second namespace from the same plugin hangs beneath it.
        nlohmann::json merge = nlohmann::json::object();
        nlohmann::json& workspace = merge["plugin"]["children"][entry.owner];
        if (entry.spec.name == entry.owner)
            workspace = std::move(node);
        else
            workspace["children"][entry.spec.name] = std::move(node);
        settings.merge_completions_json(merge);
    }
    return bound;
}

} // namespace tui

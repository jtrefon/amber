#include "slash_dispatcher.h"
#include "feed_manager.h"
#include "plugin_commands.h"
#include "tui.h"

#include <agent/data_path.h>
#include <agent/mcp_tools.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace tui {

SlashDispatcher::SlashDispatcher(Tui& tui) : tui_(tui) {}

double SlashDispatcher::compression_threshold_effective() const {
    return agent::load_compression_config(tui_.cfg_).threshold;
}

// A plugin command may return several lines (a server list, a capability table):
// each is its own status row, so nothing renders as one over-long line.
void SlashDispatcher::append_output(const std::string& text) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line =
            text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty())
            tui_.append_line(P_STATUS, line);
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
}

// Plugin completion subtrees merge additively: a plugin may extend a
// documented namespace (the MCP plugin contributes /get mcp) and add children,
// but it can never rewrite a documented field or name a core action. The
// registry merge enforces that and counts a field collision.
void SlashDispatcher::merge_plugin_completions() {
    for (const auto& p : tui_.plugins_.plugins()) {
        if (p.state == agent::PluginState::Enabled && p.manifest.completion.is_object())
            tui_.settings_.merge_completions_json(p.manifest.completion);
    }
}

void SlashDispatcher::refresh_completions() {
    tui_.settings_.reset_completion_index();
    auto try_load = [&](const std::string& path) {
        if (path.empty())
            return false;
        bool ok = tui_.settings_.load_completions_json(path);
        if (ok)
            tui_.append_line(P_DEBUG, "loaded completions from " + path);
        return ok;
    };
    std::string exe = agent::exe_path();
    for (const auto& c :
         agent::data_file_candidates("completions.json", exe.empty() ? nullptr : exe.c_str()))
        if (try_load(c))
            break;
    merge_plugin_completions();

    // Plugin command namespaces: merge each contribution and bind its
    // executable leaves into the action registry. The binding resolves the
    // handler from the live registry at dispatch time, so a disable/re-enable
    // always uses the current handler. Plugin roots come FIRST: a feed that
    // creates its namespace before the plugin that owns it would make the
    // plugin's own contribution look like a collision.
    install_plugin_commands(
        [this]() -> const std::vector<agent::CommandRegistry::Entry>& {
            return tui_.plugin_runtime_.commands().all();
        },
        tui_.settings_, action_registry_, [this](const std::string& text) { append_output(text); });

    // Live feeds add children under the namespaces they serve (MCP servers,
    // models, providers, policy rules, jobs); they never own the command node
    // itself. Merging after the plugins is what keeps the MCP command's
    // help/man/action while its server branches stay live.
    tui_.settings_.merge_completions_json(agent::mcp_completion_subtree(tui_.reg_));

    // The tree was rebuilt from scratch, so the live feeds must be merged again:
    // without this their leaves (models, providers, policy rules, jobs, plugin
    // states) disappear on any rebuild — a plugin toggle, an install, an MCP
    // connect — until the next restart.
    refresh_model_list();
    refresh_policy_feed();
    refresh_job_feed();
    refresh_provider_feed();
    if (tui_.feed_manager_) {
        tui_.feed_manager_->refresh_plugin_feed();
        tui_.feed_manager_->refresh_worktree_feed();
    }
    refresh_settings_feed();
}

void SlashDispatcher::request_quit() {
    tui_.quit_ = true;
}

} // namespace tui
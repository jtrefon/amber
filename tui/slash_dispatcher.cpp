#include "slash_dispatcher.h"
#include "tui.h"

#include <agent/data_path.h>
#include <agent/mcp_tools.h>

namespace tui {

SlashDispatcher::SlashDispatcher(Tui& tui) : tui_(tui) {}

double SlashDispatcher::compression_threshold_effective() const {
    return agent::load_compression_config(tui_.cfg_).threshold;
}

void SlashDispatcher::refresh_completions() {
    tui_.settings_.reset_completion_index();
    auto try_load = [&](const std::string& path) {
        if (path.empty()) return false;
        bool ok = tui_.settings_.load_completions_json(path);
        if (ok) tui_.append_line(P_DEBUG, "loaded completions from " + path);
        return ok;
    };
    std::string exed = agent::exe_dir();
    for (const auto& c : agent::data_file_candidates(
             "completions.json", exed.empty() ? nullptr : exed.c_str()))
        if (try_load(c)) break;
    for (const auto& p : tui_.plugins_.plugins())
        if (p.state == agent::PluginState::Enabled)
            tui_.settings_.merge_completions_json(p.manifest.completion);
    tui_.settings_.merge_completions_json(agent::mcp_completion_subtree(tui_.reg_));
}

void SlashDispatcher::request_quit() { tui_.quit_ = true; }

} // namespace tui
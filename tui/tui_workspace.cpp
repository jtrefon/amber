#include "tui/tui.h"

#include <agent/session.h>
#include <agent/workspace.h>
#include <agent/worktree.h>

#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tui {

namespace fs = std::filesystem;

namespace {

std::string join(const std::string& prefix, const std::string& name) {
    return prefix + "/" + name;
}

std::string default_name(const Window& w) {
    return "w" + std::to_string(w.id);
}

} // namespace

// A window's workspace is a git worktree of the project, created under
// $TMPDIR/amber-wt/<project-slug>/. Volatile by design: the only thing it holds
// that the repository does not is uncommitted work, and commits land in the
// repository's own object store. Branch policy belongs to the workflow library
// (fix/, feat/, bug/), so amber creates no branch — the agent does, before its
// first commit, which is also why an unreferenced HEAD is refused on removal.
//
// This is the one workspace path that forks on the UI thread, and it runs only
// for an explicit request (window creation), where the user is waiting for the
// window itself. Everything a restart needs happens in start_workspace_pass.
void Tui::prepare_window_workspace(Window& w, const std::string& requested_name) {
    if (!cfg_.worktree_auto)
        return;
    const std::string project = agent::Workspace::project_root();
    if (!agent::is_git_repo(project))
        return; // not a repository: the project root is the only workspace there is
    const std::string base = agent::git_head_oid(project);
    if (base.empty()) {
        append_line(P_STATUS, "workspace: skipped, the repository has no commit to start from");
        return;
    }
    const std::string name =
        agent::valid_worktree_name(requested_name) ? requested_name : default_name(w);
    const std::string path = join(agent::worktree_base_dir(project), name);
    std::string err;
    if (!agent::create_worktree(project, path, base, err)) {
        append_line(P_STATUS, "workspace: could not prepare " + path + " — " + err);
        return; // the window opens on the project root: preparation never blocks it
    }
    w.root = path;
    w.worktree = name;
    w.base_oid = base;
    if (w.agent)
        w.agent->set_workspace_root(path);
    agent::registry_add(agent::Workspace::state_dir(),
                        agent::make_worktree_entry(name, path, base, w.session_id));
    append_line(P_STATUS,
                "workspace: " + path + " — detached; commit to a branch before closing the window");
    // Registry and workspace.json stay in step, so a crash between them leaves
    // the reaper nothing to guess about.
    session_controller_->save_workspace_now();
}

void Tui::adopt_recorded_workspace(Window& w, const agent::WorkspaceState::WindowEntry& entry) {
    if (entry.root.empty())
        return;
    const std::string name = entry.worktree.empty() ? default_name(w) : entry.worktree;
    if (!fs::exists(entry.root)) {
        // /tmp is wiped on reboot by design; the commits are not. Queue the
        // rebuild for the background pass, which is the part that needs git.
        // The registry knows where the window last stopped; the workspace file
        // only knows where the worktree was created.
        std::string base = entry.head_oid.empty() ? entry.base_oid : entry.head_oid;
        for (const auto& known : agent::load_worktrees(agent::Workspace::state_dir())) {
            if (known.path == entry.root && !known.head_oid.empty())
                base = known.head_oid;
        }
        if (!base.empty()) {
            pending_rebuild_.push_back(
                {w.id, name, base,
                 join(agent::worktree_base_dir(agent::Workspace::project_root()), name)});
            return;
        }
    }
    w.root = entry.root;
    w.worktree = name;
    w.base_oid = entry.base_oid;
    if (w.agent)
        w.agent->set_workspace_root(entry.root);
}

void Tui::start_workspace_pass() {
    agent::WorkspacePass pass;
    pass.project = agent::Workspace::project_root();
    pass.state_dir = agent::Workspace::state_dir();
    for (const auto& w : window_manager_->all()) {
        if (w->session_id.empty() && w->root.empty())
            continue;
        pass.live_sessions.push_back(w->session_id);
        if (!w->root.empty())
            pass.live_paths.push_back(w->root);
    }
    for (const auto& pending : pending_rebuild_)
        pass.rebuilds.push_back({pending.window_id, pending.name, pending.base, pending.path});
    pending_rebuild_.clear();

    auto poster = ui_poster(); // dropped once destruction begins: never a torn Tui
    std::thread([this, pass, poster] {
        const agent::WorkspacePassResult result = agent::run_workspace_pass(pass);
        poster([this, rebuilt = result.rebuilt, notices = result.notices] {
            apply_workspace_result(rebuilt, notices);
        });
    }).detach();
}

void Tui::apply_workspace_result(const std::vector<std::pair<size_t, std::string>>& rebuilt,
                                 const std::vector<std::string>& notices) {
    for (const auto& entry : rebuilt) {
        Window* w = window_manager_->by_id(entry.first);
        if (!w)
            continue;
        w->root = entry.second;
        if (w->agent)
            w->agent->set_workspace_root(entry.second);
    }
    for (const auto& line : notices)
        append_line(P_STATUS, line);
    if (!notices.empty() || !rebuilt.empty())
        draw();
}

void Tui::release_window_workspace(Window& w) {
    if (w.root.empty())
        return;
    pending_workspace_notice_ = agent::release_workspace(
        agent::Workspace::project_root(), agent::Workspace::state_dir(), w.worktree, w.root);
}

void Tui::flush_workspace_notice() {
    if (pending_workspace_notice_.empty())
        return;
    // The active window may be the welcome mural, which renders art instead of
    // scrollback: hold the notice until a window that shows lines is active, or
    // it would be written where nobody can read it.
    if (win().welcome_art)
        return;
    append_line(P_STATUS, pending_workspace_notice_);
    pending_workspace_notice_.clear();
}

std::string Tui::active_workspace_root() const {
    const Window& w = win();
    return w.root.empty() ? agent::Workspace::project_root() : w.root;
}

} // namespace tui

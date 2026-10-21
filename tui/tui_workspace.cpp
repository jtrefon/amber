#include "tui/tui.h"
#include "tui/feed_manager.h"

#include <agent/session.h>
#include <agent/workspace.h>
#include <agent/worktree.h>

#include <algorithm>
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

struct LeaveOutcome {
    bool ok = false;
    std::vector<std::string> messages;
};

// The dirty path: never force a removal, preserve the work in the repository's
// stash under a message that names the workspace.
LeaveOutcome stash_and_leave(const agent::WorktreeStatus& status, const std::string& name,
                             const std::string& path, const std::string& project) {
    const int pick = menu_select("worktree " + name + ": " + status.problem + ". Leave it?",
                                 {"Cancel", "Stash and leave"});
    if (pick != 1)
        return {false, {"worktree: kept " + name + " — " + status.problem}};
    std::string err;
    if (!agent::stash_worktree(project, path, "amber: leave " + name, err))
        return {false, {"worktree: kept " + name + " — could not stash: " + err}};
    return {true, {"worktree: changes stashed (git stash list keeps them)"}};
}

// The unreferenced-HEAD path: the only automatic branch amber will create is
// the one the user explicitly chooses here. Its name names the workspace and
// the commit, so `git branch` lists something recognizable later.
LeaveOutcome branch_and_leave(const agent::WorktreeStatus& status, const std::string& name,
                              const std::string& path, const std::string& project) {
    const std::string short_oid = status.head_oid.substr(0, 8);
    const std::string branch = "amber-leave-" + name + "-" + short_oid;
    const int pick =
        menu_select("worktree " + name + ": HEAD " + short_oid + " is on no branch. Leave it?",
                    {"Cancel", "Create branch " + branch + " and leave"});
    if (pick != 1)
        return {false,
                {"worktree: kept " + name + " — commits at " + short_oid + " reach no branch"}};
    std::string err;
    if (!agent::create_branch_at_head(path, branch, err))
        return {false, {"worktree: kept " + name + " — " + err}};
    return {true, {"worktree: commits saved on branch " + branch}};
}

LeaveOutcome confirm_leave(const std::string& name, const std::string& path,
                           const std::string& project) {
    const int pick =
        menu_select("worktree " + name + ": leave " + path + " and return to " + project +
                        "? The directory is removed; commits stay in the repository.",
                    {"Cancel", "Leave and remove"});
    if (pick != 1)
        return {false, {"worktree: kept " + name}};
    return {true, {}};
}

// Walk the leave decision in the order that can lose the least: stash first
// (uncommitted work), then give a detached HEAD a branch, then the removal
// confirmation. A step the user cancels aborts the whole leave.
LeaveOutcome prepare_leave(const agent::WorktreeStatus& status, const std::string& name,
                           const std::string& path, const std::string& project) {
    LeaveOutcome outcome;
    if (status.dirty) {
        outcome = stash_and_leave(status, name, path, project);
        if (!outcome.ok)
            return outcome;
    }
    if (!status.reachable) {
        LeaveOutcome branch = branch_and_leave(status, name, path, project);
        if (!branch.ok)
            return branch;
        outcome.ok = true;
        outcome.messages.insert(outcome.messages.end(), branch.messages.begin(),
                                branch.messages.end());
        return outcome;
    }
    if (status.dirty)
        return outcome; // the stash dialog already chose leave
    return confirm_leave(name, path, project);
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
                        agent::make_worktree_entry(name, path, base, worktree_claim(w)));
    append_line(P_STATUS,
                "workspace: " + path + " — detached; commit to a branch before closing the window");
    // Registry and workspace.json stay in step, so a crash between them leaves
    // the reaper nothing to guess about.
    session_controller_->save_workspace_now();
    // The new name is a live leaf under /worktree attach|remove immediately,
    // not after the next restart.
    refresh_worktree_feed();
}

void Tui::park_restored_workspace(const std::string& name) {
    agent::park_worktree(agent::Workspace::state_dir(), name);
    pending_workspace_notice_ = "worktree: off — window at the project root; workspace " + name +
                                " parked (attach with /worktree attach " + name + ")";
}

void Tui::queue_restored_rebuild(Window& w, const agent::WorkspaceState::WindowEntry& entry,
                                 const std::string& name) {
    // /tmp is wiped on reboot by design; the commits are not. Queue the rebuild
    // for the background pass, which is the part that needs git. The registry
    // knows where the window last stopped; the workspace file only knows where
    // the worktree was created.
    std::string base = entry.head_oid.empty() ? entry.base_oid : entry.head_oid;
    for (const auto& known : agent::load_worktrees(agent::Workspace::state_dir()))
        if (known.path == entry.root && !known.head_oid.empty())
            base = known.head_oid;
    if (base.empty()) {
        pending_workspace_notice_ =
            "worktree: " + name + " is gone and has no recorded base; window at the project root";
        return;
    }
    pending_rebuild_.push_back(
        {w.id, name, base, join(agent::worktree_base_dir(agent::Workspace::project_root()), name)});
}

bool Tui::claim_restored_workspace(Window& w, const std::string& name,
                                   const std::string& worktree) {
    if (worktree.empty())
        return true;
    std::string err;
    if (agent::claim_worktree(agent::Workspace::state_dir(), name, worktree_claim(w),
                              live_worktree_claims(), err))
        return true;
    pending_workspace_notice_ =
        "worktree: " + name + " not re-attached — " + err + "; window at the project root";
    return false;
}

void Tui::adopt_recorded_workspace(Window& w, const agent::WorkspaceState::WindowEntry& entry) {
    if (entry.root.empty() && entry.worktree.empty())
        return;
    const std::string name = entry.worktree.empty() ? default_name(w) : entry.worktree;
    // `worktree off` means off, including for a window that was saved while it
    // owned one. Park rather than re-attach: the user asked not to work in a
    // worktree, and parking keeps the directory (and any uncommitted work) for
    // an explicit `/worktree attach`.
    if (!cfg_.worktree_auto) {
        if (!entry.worktree.empty())
            park_restored_workspace(name);
        return;
    }
    if (!fs::exists(entry.root)) {
        queue_restored_rebuild(w, entry, name);
        return;
    }
    if (!claim_restored_workspace(w, name, entry.worktree))
        return;
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
    pass.live_sessions = live_worktree_claims();
    pass.live_paths = live_worktree_paths();
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
        // The registry row the background pass just wrote carries no session;
        // claim it now, or the workspace reads as unowned until the next run.
        for (const auto& known : agent::load_worktrees(agent::Workspace::state_dir())) {
            if (known.path != entry.second)
                continue;
            w->worktree = known.name;
            w->base_oid = known.base_oid;
            std::string err;
            (void)agent::claim_worktree(agent::Workspace::state_dir(), known.name,
                                        worktree_claim(*w), live_worktree_claims(), err);
            break;
        }
        if (w->agent)
            w->agent->set_workspace_root(entry.second);
    }
    for (const auto& line : notices)
        append_line(P_STATUS, line);
    if (!notices.empty() || !rebuilt.empty()) {
        refresh_worktree_feed();
        draw();
    }
}

void Tui::release_window_workspace(Window& w) {
    if (w.root.empty())
        return;
    pending_workspace_notice_ = agent::release_workspace(
        agent::Workspace::project_root(), agent::Workspace::state_dir(), w.worktree, w.root);
    // A workspace that could not be removed is dirty or detached: park it so
    // the reaper keeps it for a later explicit decision instead of treating
    // the closing session as an abandoned owner.
    if (pending_workspace_notice_.rfind("workspace kept", 0) == 0 && !w.worktree.empty())
        agent::park_worktree(agent::Workspace::state_dir(), w.worktree);
}

bool Tui::workspace_leave_allowed(Window& w) {
    if (w.worktree.empty() && w.root.empty()) {
        append_line(P_STATUS, "worktree: this window is already at the project root");
        return false;
    }
    if (runs_.busy(w.id)) {
        append_line(P_STATUS, "worktree: the window is running — stop it first");
        return false;
    }
    return true;
}

void Tui::leave_vanished_workspace(Window& w, const std::string& name, const std::string& path,
                                   const std::string& project, const std::string& state) {
    if (!name.empty())
        agent::registry_remove(state, name);
    detach_window_from_workspace(w, /*park=*/false);
    append_line(P_STATUS, "worktree: left " + (name.empty() ? path : name) +
                              " (directory was already gone); window at " + project);
    refresh_worktree_feed();
    draw();
}

bool Tui::leave_window_workspace(Window& w) {
    if (!workspace_leave_allowed(w))
        return false;
    const std::string project = agent::Workspace::project_root();
    const std::string state = agent::Workspace::state_dir();
    const std::string name = w.worktree;
    const std::string path = w.root;

    std::error_code ec;
    const auto entries = agent::load_worktrees(state);
    const auto found =
        std::find_if(entries.begin(), entries.end(), [&](const agent::WorktreeEntry& e) {
            return (!name.empty() && e.name == name) || (!path.empty() && e.path == path);
        });
    if (name.empty() || path.empty() || found == entries.end() || !fs::exists(path, ec)) {
        leave_vanished_workspace(w, name, path, project, state);
        return true;
    }

    const agent::WorktreeStatus status =
        agent::worktree_status(project, *found, live_worktree_claims(), live_worktree_paths());
    const LeaveOutcome outcome = prepare_leave(status, name, path, project);
    for (const auto& message : outcome.messages)
        append_line(P_STATUS, message);
    if (!outcome.ok) {
        draw();
        return false;
    }
    std::string err;
    if (!agent::remove_worktree_safe(project, path, err)) {
        append_line(P_STATUS, "worktree: kept " + name + " — " + err);
        draw();
        return false;
    }
    agent::registry_remove(state, name);
    detach_window_from_workspace(w, /*park=*/false);
    append_line(P_STATUS, "worktree: left " + name + "; window now at " + project);
    refresh_worktree_feed();
    draw();
    return true;
}

void Tui::detach_window_from_workspace(Window& w, bool park) {
    const std::string name = w.worktree;
    if (park && !name.empty())
        agent::park_worktree(agent::Workspace::state_dir(), name);
    w.root.clear();
    w.worktree.clear();
    w.base_oid.clear();
    if (w.agent)
        w.agent->set_workspace_root("");
    // The conversation belongs to the window, not the directory: sessions are
    // saved as they are, so leaving never loses history or scrollback.
    session_controller_->save_window_sessions();
    session_controller_->save_workspace_now();
}

std::string Tui::worktree_claim(const Window& w) const {
    // A session id exists only after the first save. The per-window id is
    // stable for the run and lets the registry tell two unsaved windows apart.
    return w.session_id.empty() ? "window-" + std::to_string(w.id) : w.session_id;
}

std::vector<std::string> Tui::live_worktree_claims() const {
    std::vector<std::string> claims;
    for (const auto& w : window_manager_->all())
        claims.push_back(worktree_claim(*w));
    return claims;
}

std::vector<std::string> Tui::live_worktree_paths() const {
    std::vector<std::string> paths;
    for (const auto& w : window_manager_->all())
        if (!w->root.empty())
            paths.push_back(w->root);
    return paths;
}

void Tui::refresh_worktree_feed() {
    if (feed_manager_)
        feed_manager_->refresh_worktree_feed();
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

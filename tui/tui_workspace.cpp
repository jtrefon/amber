#include "tui/tui.h"

#include <agent/session.h>
#include <agent/worktree.h>
#include <agent/workspace.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
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

// What the background pass needs, as plain data: it runs on its own thread and
// must not touch the Tui (or any Window) while the UI keeps working.
struct Plan {
    std::string project;
    std::string state_dir;
    std::vector<std::string> live_sessions;
    std::vector<std::string> live_paths;
    struct Rebuild {
        size_t window_id = 0;
        std::string name;
        std::string base;
        std::string path;
    };
    std::vector<Rebuild> rebuilds;
};

struct Outcome {
    std::vector<std::string> notices;
    std::vector<std::pair<size_t, std::string>> rebuilt; // window id -> path
};

void reap_into(const Plan& plan, Outcome& out) {
    const agent::ReapReport report =
        agent::reap_worktrees(plan.project, plan.state_dir, plan.live_sessions, plan.live_paths);
    for (const auto& name : report.removed)
        out.notices.push_back("workspace cleaned up: " + name);
    for (const auto& kept : report.kept)
        out.notices.push_back("workspace kept: " + kept.first + " \u2014 " + kept.second);
}

void rebuild_into(const Plan& plan, Outcome& out) {
    for (const auto& r : plan.rebuilds) {
        std::string err;
        if (agent::create_worktree(plan.project, r.path, r.base, err)) {
            agent::registry_add(plan.state_dir,
                                agent::make_worktree_entry(r.name, r.path, r.base, ""));
            out.rebuilt.emplace_back(r.window_id, r.path);
            out.notices.push_back("workspace rebuilt at " + r.path);
            continue;
        }
        out.notices.push_back("workspace " + r.name +
                              " could not be rebuilt \u2014 using the project root");
    }
}

// Where each live workspace stopped, so the next rebuild lands there rather
// than at the commit it was created from.
void refresh_heads(const Plan& plan) {
    std::vector<agent::WorktreeEntry> entries = agent::load_worktrees(plan.state_dir);
    bool changed = false;
    for (auto& entry : entries) {
        const bool live = std::find(plan.live_paths.begin(), plan.live_paths.end(), entry.path) !=
                          plan.live_paths.end();
        if (!live || !fs::exists(entry.path))
            continue;
        const std::string head = agent::git_head_oid(entry.path);
        if (!head.empty() && head != entry.head_oid) {
            entry.head_oid = head;
            changed = true;
        }
    }
    if (changed)
        agent::save_worktrees(plan.state_dir, entries);
}

// All the git work, on a worker thread.
Outcome run_pass(const Plan& plan) {
    Outcome out;
    if (!agent::is_git_repo(plan.project))
        return out;
    reap_into(plan, out);
    rebuild_into(plan, out);
    refresh_heads(plan);
    return out;
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
        append_line(P_STATUS, "workspace: could not prepare " + path + " \u2014 " + err);
        return; // the window opens on the project root: preparation never blocks it
    }
    w.root = path;
    w.worktree = name;
    w.base_oid = base;
    if (w.agent)
        w.agent->set_workspace_root(path);
    agent::registry_add(agent::Workspace::state_dir(),
                        agent::make_worktree_entry(name, path, base, w.session_id));
    append_line(P_STATUS, "workspace: " + path +
                              " \u2014 detached; commit to a branch before closing the window");
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
        const std::string base = entry.head_oid.empty() ? entry.base_oid : entry.head_oid;
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
    Plan plan;
    plan.project = agent::Workspace::project_root();
    plan.state_dir = agent::Workspace::state_dir();
    for (const auto& w : window_manager_->all()) {
        if (w->session_id.empty() && w->root.empty())
            continue;
        plan.live_sessions.push_back(w->session_id);
        if (!w->root.empty())
            plan.live_paths.push_back(w->root);
    }
    for (const auto& pending : pending_rebuild_)
        plan.rebuilds.push_back({pending.window_id, pending.name, pending.base, pending.path});
    pending_rebuild_.clear();

    auto poster = ui_poster(); // dropped once destruction begins: never a torn Tui
    std::thread([this, plan, poster] {
        const Outcome outcome = run_pass(plan);
        poster([this, outcome] {
            for (const auto& rebuilt : outcome.rebuilt) {
                if (Window* w = window_manager_->by_id(rebuilt.first)) {
                    w->root = rebuilt.second;
                    if (w->agent)
                        w->agent->set_workspace_root(rebuilt.second);
                }
            }
            for (const auto& line : outcome.notices)
                append_line(P_STATUS, line);
            if (!outcome.notices.empty() || !outcome.rebuilt.empty())
                draw();
        });
    }).detach();
}

void Tui::release_window_workspace(Window& w) {
    if (w.root.empty())
        return;
    std::string err;
    if (agent::remove_worktree_safe(agent::Workspace::project_root(), w.root, err)) {
        agent::registry_remove(agent::Workspace::state_dir(), w.worktree);
        pending_workspace_notice_ = "workspace removed: " + w.root;
    } else {
        pending_workspace_notice_ = "workspace kept at " + w.root + " \u2014 " + err;
    }
}

void Tui::flush_workspace_notice() {
    if (pending_workspace_notice_.empty())
        return;
    append_line(P_STATUS, pending_workspace_notice_);
    pending_workspace_notice_.clear();
}

std::string Tui::active_workspace_root() const {
    const Window& w = win();
    return w.root.empty() ? agent::Workspace::project_root() : w.root;
}

} // namespace tui

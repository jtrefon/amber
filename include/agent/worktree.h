#ifndef AGENT_WORKTREE_H
#define AGENT_WORKTREE_H

#include <string>
#include <utility>
#include <vector>

namespace agent {

// One amber-owned worktree, as persisted in the registry.
struct WorktreeEntry {
    std::string name;          // validated; also the directory name under the base
    std::string path;          // where the working copy lives
    std::string base_oid;      // the commit it was created at
    std::string head_oid;      // last observed HEAD; informational
    std::string session_id;    // the live window that claims it; empty when parked
    long pid = 0;              // the process that created/claimed it
    long long start_ticks = 0; // /proc/<pid>/stat starttime: guards pid reuse
    long long created_ms = 0;
    // Parked is an explicit "kept, but nobody owns it". A parked workspace is
    // never reaped: its contents are what the user chose to keep. It can be
    // claimed again by name.
    bool parked = false;
};

// What the user sees for one workspace. `state` is live (claimed by a window),
// parked (kept, unclaimed), or kept (unclaimed and unsafe to remove, because it
// is dirty or its HEAD is reachable from no ref).
enum class WorktreeState { Live, Parked, Kept };

struct WorktreeStatus {
    std::string name;
    std::string path;
    std::string branch;   // current branch, or "detached"
    std::string head_oid; // short or full HEAD
    std::string owner;    // claiming session id, or the pid that created it
    std::string problem;  // why it cannot be removed; empty when safe
    bool exists = false;
    bool dirty = false;
    bool reachable = true;
    bool parked = false;
    bool live = false;
    WorktreeState state = WorktreeState::Kept;
};

// Branch, cleanliness and reachability of one workspace. The dirtiness reason
// is a short human sentence ("3 modified, 1 untracked"), never a raw diff.
std::string worktree_dirty_reason(const std::string& path);
std::string worktree_branch(const std::string& path, std::string& head_oid);
bool worktree_head_reachable(const std::string& repo, const std::string& path);
WorktreeStatus worktree_status(const std::string& repo, const WorktreeEntry& entry,
                               const std::vector<std::string>& live_sessions,
                               const std::vector<std::string>& live_paths);
std::vector<WorktreeStatus> worktree_statuses(const std::string& repo,
                                              const std::vector<WorktreeEntry>& entries,
                                              const std::vector<std::string>& live_sessions,
                                              const std::vector<std::string>& live_paths);

// Resolve a ref (branch, tag, HEAD~2) to the commit it names, or "".
std::string git_resolve_commit(const std::string& repo, const std::string& ref);

// Give a detached HEAD a branch so its commits survive removal. Used only by
// the explicit leave dialog; amber never invents a branch on its own.
bool create_branch_at_head(const std::string& path, const std::string& branch, std::string& error);

// Preserve uncommitted work in the repository's stash before leaving. The
// message names the workspace so `git stash list` still says where it came
// from. A tree with nothing to save is a success.
bool stash_worktree(const std::string& path, const std::string& message, std::string& error);

// A worktree is usable as a workspace only for a name that stays a single,
// visible path component: separators, traversal and hidden names are refused
// because the name becomes a directory under the worktree base dir.
bool valid_worktree_name(const std::string& name);

// Where a project's worktrees live: "$TMPDIR/amber-wt/<project-slug>".
// Volatile by design: a worktree holds uncommitted work until it is committed,
// and commits land in the repository's own object store, which is on disk.
std::string worktree_base_dir(const std::string& project_root);

// Git questions, answered by the git binary. Arguments are passed as argv,
// never through a shell: names reach here from the model. A missing repository
// is a false/empty answer, not an error.
bool is_git_repo(const std::string& dir);
std::string git_head_oid(const std::string& dir);

// One prompt line for an agent whose workspace is a DETACHED worktree: the
// branch it must create before its first commit, because this window refuses to
// discard commits no branch reaches. Empty for the main checkout, for a
// worktree already on a branch, and for a non-repository.
std::string worktree_session_note(const std::string& root);

// Repository-relative paths matching `glob`, from tracked files plus untracked
// files that are not ignored. Empty when `dir` is not a repository. Git does not
// descend into a nested repository or linked worktree, which is what keeps a
// sibling checkout out of a scan; the caller still filters what it wants.
std::vector<std::string> git_files_matching(const std::string& dir, const std::string& glob);

// Create a DETACHED worktree at `base_oid`. Branch policy belongs to the
// workflow (fix/, feat/, bug/, ...), so amber creates no branch: the agent
// does, before its first commit. Fails when the path exists and is not empty.
bool create_worktree(const std::string& repo, const std::string& path, const std::string& base_oid,
                     std::string& error);

// Remove a worktree only when nothing can be lost. Git refuses a dirty tree
// (modified OR untracked files); this refuses additionally when HEAD's commits
// are reachable from no ref, because removing the worktree would leave them to
// expire. A path that is already gone drops the repository's stale metadata
// and counts as removed.
bool remove_worktree_safe(const std::string& repo, const std::string& path, std::string& error);

// Drop metadata for worktree directories that no longer exist.
void prune_worktrees(const std::string& repo);

// The kernel start time of a process, which is what tells a live owner from a
// reused pid. -1 when the process, or /proc, is unavailable.
long long process_start_ticks(long pid);

enum class OwnerState { Alive, Dead, Unknown };

// Liveness of the process that created a worktree. "Unknown" is returned when
// no start-time stamp is available (a registry written on a platform without
// procfs), and it never authorizes a removal.
OwnerState owner_state(const WorktreeEntry& entry);

// The registry: <state_dir>/worktrees.json, written atomically (tmp + rename)
// like the session store. A missing or damaged file reads as empty.
std::vector<WorktreeEntry> load_worktrees(const std::string& state_dir);
bool save_worktrees(const std::string& state_dir, const std::vector<WorktreeEntry>& entries);

// Build the registry entry for a worktree that was just created. The owner
// stamp — pid plus its kernel start time — is what a later run uses to tell a
// live window from a reused pid, and it cannot be reconstructed afterwards.
WorktreeEntry make_worktree_entry(const std::string& name, const std::string& path,
                                  const std::string& base_oid, const std::string& session_id);

// Registry edits the window lifecycle needs, so no caller rewrites the file by
// hand: add replaces the entry carrying the same name, remove drops it by name
// and reports whether it was there.
bool registry_add(const std::string& state_dir, const WorktreeEntry& entry);
bool registry_remove(const std::string& state_dir, const std::string& name);

// Claim a workspace for `session_id`. A session that is still live in another
// window owns it: claiming refuses rather than stealing ("no silent attach").
// A stale session id (the process that wrote it is gone) is reclaimable.
bool claim_worktree(const std::string& state_dir, const std::string& name,
                    const std::string& session_id, const std::vector<std::string>& live_sessions,
                    std::string& error);

// Release the claim without touching the directory: the workspace becomes
// parked and is never reaped.
bool park_worktree(const std::string& state_dir, const std::string& name);

// Drop registry entries whose directories have vanished. Parked entries whose
// directory still exists are kept; a vanished directory leaves nothing to keep.
// Returns the names removed, so the caller can report them.
std::vector<std::string> prune_registry(const std::string& repo, const std::string& state_dir);

// What a reap pass did, in the terms the UI reports to the user.
struct ReapReport {
    std::vector<std::string> removed;                      // names
    std::vector<std::pair<std::string, std::string>> kept; // name -> why it stayed
};

// The background workspace pass, as plain data: the caller says what the live
// windows claim and what has to be rebuilt, and gets back the lines to show and
// the rebuilds that succeeded. Everything here forks git, so it belongs on a
// worker — the UI thread must never wait on a subprocess.
struct WorkspacePass {
    struct Rebuild {
        size_t window_id = 0; // for the caller to match the result to a window
        std::string name;
        std::string base; // the commit to rebuild at
        std::string path;
    };
    std::string project;
    std::string state_dir;
    std::vector<std::string> live_sessions;
    std::vector<std::string> live_paths;
    std::vector<Rebuild> rebuilds;
};

struct WorkspacePassResult {
    std::vector<std::string> notices;
    std::vector<std::pair<size_t, std::string>> rebuilt; // window id -> path
};

WorkspacePassResult run_workspace_pass(const WorkspacePass& pass);

// Give a window's workspace back: remove it when nothing can be lost (git
// refuses a dirty tree, and an unreferenced HEAD is refused by name) and drop
// its registry entry. Returns the line to show the user either way.
std::string release_workspace(const std::string& project, const std::string& state_dir,
                              const std::string& name, const std::string& path);

// Remove worktrees whose owner is provably gone and which nothing claims.
// A live session id claims one; so does a path a restored window is about to
// re-attach to — the path matters because the registry entry may predate the
// session id, and reaping a workspace out from under a restored window is the
// one failure this whole mechanism exists to prevent. Entries whose owner
// cannot be judged (no procfs, no usable stamp) are left alone — "cannot tell"
// never authorizes a removal. An entry whose directory has already vanished is
// dropped and pruned: there is nothing left to lose, and the repository's
// stale metadata should go with it. A candidate that cannot be removed safely
// is kept WITH its reason, so the next run reports it again instead of losing
// it in a log line.
ReapReport reap_worktrees(const std::string& repo, const std::string& state_dir,
                          const std::vector<std::string>& live_sessions,
                          const std::vector<std::string>& live_paths);

} // namespace agent

#endif // AGENT_WORKTREE_H

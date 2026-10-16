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
    std::string session_id;    // the window session it belongs to
    long pid = 0;              // the process that created it
    long long start_ticks = 0; // /proc/<pid>/stat starttime: guards pid reuse
    long long created_ms = 0;
};

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

// What a reap pass did, in the terms the UI reports to the user.
struct ReapReport {
    std::vector<std::string> removed;                      // names
    std::vector<std::pair<std::string, std::string>> kept; // name -> why it stayed
};

// Remove worktrees whose owner is gone and which no live session references.
// Alive owners and live sessions are left alone silently; a candidate that
// cannot be removed safely is kept WITH its reason, so the next run reports it
// again instead of losing it in a log line.
ReapReport reap_worktrees(const std::string& repo, const std::string& state_dir,
                          const std::vector<std::string>& live_sessions);

} // namespace agent

#endif // AGENT_WORKTREE_H

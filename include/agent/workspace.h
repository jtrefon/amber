
#ifndef AGENT_WORKSPACE_H
#define AGENT_WORKSPACE_H

#include <string>

namespace agent {

// Path confinement for filesystem-touching tools.
//
// Tools may only read or write inside a "workspace root": by default the
// directory amber was started in (overridable with AMBER_WORKSPACE, or set
// explicitly for tests), or — while a window works in its own git worktree —
// that worktree, resolved per RUN through the run scope. Resolving per run is
// what lets two windows confine to different trees while a tool running on a
// dispatch worker still inherits the dispatching agent's root.
//
// The PROJECT root is a separate question: state, project config, policy and
// the shared skill/memory pools belong to the project every window has in
// common, worktree windows included. A worktree is a working copy, not a
// second project, so those stay keyed to the directory amber was started in.
//
// This is a defense-in-depth measure: the model driving the agent should not be
// able to escape the workspace via absolute paths (e.g. "/etc/passwd") or
// traversal ("../../../etc/shadow").
class Workspace {
public:
    // The PROJECT root as an absolute, normalized path: what state_dir(),
    // local_dir(), settings, policy and the shared pools are keyed to. A run
    // scope or a UI ScopedProjectRoot overrides the process default.
    static std::string project_root();

    // The root the running agent's tools are confined to: the active run's
    // worktree override when one is installed, else project_root().
    static std::string workspace_root();

    // The process default, ignoring UI/run scopes. Used when a window has no
    // project root of its own.
    static std::string process_project_root();

    // Override the process-default project root (tests and startup).
    static void set_root(const std::string& path);

    // Project-scoped path resolvers. The no-argument forms above use the
    // current scope; these let a host address a window's project explicitly.
    static std::string local_dir_for(const std::string& project_root);
    static std::string state_dir_for(const std::string& project_root);
    static std::string settings_path_for(const std::string& project_root);
    static std::string policy_path_for(const std::string& project_root);

    // The project-local CONFIG directory: "<project root>/.amber". Holds what a
    // project shares with its collaborators (skills, MCP servers, plugins) --
    // never private state. Not created on access: a read-only run must not
    // leave a directory behind, and running amber in $HOME used to create a
    // project dir there just by starting up.
    static std::string local_dir();

    // The per-project STATE directory:
    //   $XDG_STATE_HOME/amber/projects/<slug>, else
    //   ~/.local/state/amber/projects/<slug>
    // Sessions, local settings, logs, experience and policy live here: state
    // is this user's machine-local history, not something the repository
    // shares, and keeping it in the project tree meant running amber in $HOME
    // scattered <home>/.amber next to the global config. Not created on
    // access; callers create what they write.
    static std::string state_dir();

    // Path of the project's local settings file, adopting a legacy
    // .amber/settings copy the first time (see adopt_legacy_state).
    static std::string settings_path();

    // Path of the project's remembered-approval policy file, adopting a legacy
    // .amber/policy.json copy the first time.
    static std::string policy_path();

    // Copy a pre-state-layout artefact named `name` (file or directory) from
    // <root>/.amber into the state dir, once, when the state copy is absent.
    // Copy, never move: a layout change is not a reason to delete data the
    // user may still be reading, and a crash mid-copy must leave them with the
    // working old copy rather than neither.
    static void adopt_legacy_state(const std::string& name);

    // Resolve `path` (absolute or relative to the root) and verify it stays
    // within the root. On success `resolved` is the absolute normalized path
    // and the function returns true. On violation it returns false and fills
    // `error` with a message suitable for returning to the model.
    static bool confine(const std::string& path, std::string& resolved, std::string& error);

    // Strip the workspace root prefix from `path`, returning a relative path.
    // If `path` is not under the root, returns it unchanged.
    static std::string relative(const std::string& path);
};

// Install a project root for the current thread's UI work. Commands, worktree
// management and session paths resolve against it; a nested run scope wins.
class ScopedProjectRoot {
public:
    explicit ScopedProjectRoot(std::string root);
    ~ScopedProjectRoot();
    ScopedProjectRoot(const ScopedProjectRoot&) = delete;
    ScopedProjectRoot& operator=(const ScopedProjectRoot&) = delete;

private:
    std::string root_;
    const std::string* prev_ = nullptr;
};

// The stable, filesystem-safe directory name for one workspace path.
//
// Readable (the path with separators folded to '-') so a user can tell which
// directory belongs to which project, and collision-free (a short hash of the
// FULL path always rides along) so /a-b/c and /a/b-c -- which sanitize to the
// same name -- cannot silently share sessions. Pure: unit-testable without a
// filesystem.
std::string workspace_state_slug(const std::string& workspace_path);

} // namespace agent

#endif // AGENT_WORKSPACE_H

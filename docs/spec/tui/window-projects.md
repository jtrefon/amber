# Spec: Per-window project roots and project sessions

- **Status:** Core implemented in the working tree (2026-10-22): per-window
  project root, run-scope propagation, per-project session stores/settings/
  policy/instructions, `/project list|open|show|close`, missing-project restore
  validation, and a project-scoped worktree rebuild/reap pass. `workspace.json`
  remains a host-level window manifest (entries carry `project_root`); moving
  it per project is a follow-up. The Work board remains a snapshot modal v1.
- **Tracking:** #321; scoped roots #329; project UX #330; release #331.
- **Related:** `command-namespace.md`, `work-surfaces.md`, `../plugins/plugin-framework.md`,
  `docs/fix-proposal/window-worktrees-2026-10-16.md`.

## 1. Problem

A window can own a **git worktree**, but every window is still a worktree (or
the root) of the one project the process started in. That is not a project
session.

`Workspace::project_root()` is a cached process-global. Sessions, settings,
policy, experience, project instructions, project index, skills and the worktree
registry are all derived from it. A second project in the same TUI would
silently reuse the first project’s state, instructions and worktree registry.

Opening a different project must be a first-class context, not a path override
on one tool call.

## 2. Model

Add a project axis beside the existing workspace axis:

| Field | Meaning |
|---|---|
| `project_root` | the selected project directory; empty means the process project root |
| `workspace_root` | the tree the tools operate in: project root or a worktree of it |

- `Window::project_root`.
- `Agent::set_project_root(const std::string&)`.
- `RunScope::project_root` (non-owning pointer, like `workspace_root`).
- `ScopedRunScope` inheritance: a nested run keeps the project root it
  inherited; a host installs a new one.
- `Workspace::project_root()` remains the process default for hosts; path
  resolvers use the scoped project root when present.
- `WorkspaceState::WindowEntry` gains `project_root` and a stable
  `project_id`/path; restore validates that the saved path still exists and is
  the same project, never silently adopts another tree.

A project session is a chat window whose `project_root` is set. A worktree
session is a project session whose `workspace_root` is a worktree of that
project.

## 3. Per-project state

Every path that currently derives from `project_root()` must be keyed by the
window’s project:

- session store and `workspace.json` (or a project-scoped session directory);
- state/settings/policy/experience paths;
- worktree registry (`worktrees.json`);
- project instructions (`AGENTS.md`) cache — key by project root, not a
  single-slot cache;
- project index/title/cache;
- skills/memory pools (the `WindowManager` maps are already keyed by a string;
  use the project root, not the constant);
- project config (`amber.conf`/local settings) and prompt paths where they are
  project-scoped;
- project-specific MCP/plugin state if a plugin declares project scope.

Global provider/model/secret config remains global. The split is: **provider
and credentials global; project files, sessions, instructions, index, worktrees
and project state per project.**

## 4. Worktrees

`create_worktree(repo, path, base)`, `worktree_base_dir(project)`,
`claim_worktree`, `park_worktree` already take paths at the library boundary.
The TUI callers currently pass `Workspace::project_root()`; they must pass the
window’s project. Worktree names remain project-scoped.

Opening a project does not create a worktree automatically. The existing
`worktree_auto` policy may create one for the project session, exactly as it does
for a same-project window.

## 5. Commands and UX

Canonical:

```text
/project list
/project open <path> [--worktree <name>] [--no-worktree]
/project show
/project close
```

- `/project open` creates a new chat window with that project root, inheriting
  the process model/provider config but loading project-local settings,
  instructions, sessions and worktree registry.
- The Work board has a Project column and a “new project session” action.
- `/window` remains the surface namespace; a project session is a chat window.
- The project path is shown in the window title/prompt/status (not only in the
  first line).
- `@file`, `/files`, `/system`, `/job start` and all UI-thread file operations
  resolve the **active window’s project/workspace**, not the process root.
  This also fixes the existing worktree inconsistency.
- Project-scoped commands (`/session`, `/worktree`, `/project`) operate on the
  active window’s project.

## 6. Security and approval

- Opening a project is an explicit action with the path shown; no implicit path
  adoption on restore.
- Tools remain confined to the window’s `workspace_root`; a project root is
  validated and canonicalized before use.
- A project session does not inherit another project’s AGENTS.md, skills,
  memories, index or worktree claims.
- Global credentials are never copied into a project file.
- If a restored project path is missing or has changed identity, open the
  session read-only/closed with a clear notice rather than selecting another
  project.

## 7. Migration and blockers

Current blockers, in order:

1. `Workspace::project_root()` is process-global and used by all state paths.
2. No project identity on `Window`/`WorkspaceState`.
3. One `SessionStore` per Tui and one `workspace.json`.
4. Worktree creation hardcodes the process project.
5. Skill/memory/instructions/index caches use the process root.
6. UI-thread file/command surfaces bypass the per-run workspace.
7. Process-global ToolRegistry/plugin runtime/MCP config is not keyed by
   project; project-scoped plugin state needs an explicit boundary.

Do not hide this work inside the Work board PR. It is a core state migration
and needs its own tests, especially restore and “two projects, two windows.”

## 8. Tests

- **PRJ-01** two project windows have distinct project roots, state dirs,
  session lists, worktree registries and AGENTS.md blocks.
- **PRJ-02** a project window's tools, `@file`, `/files`, `/system` and
  `/job start` resolve its root, not the process root.
- **PRJ-03** a worktree of project B is created/attached under B, never under A.
- **PRJ-04** restart restores project identity and rejects a missing/changed
  path without silently adopting another project.
- **PRJ-05** global provider/key config still applies to both projects.
- **PRJ-06** a project session closing does not release another project’s
  worktree or session.
- **PRJ-07** PTY: open a second project, run a command/read a file, switch back,
  and verify both contexts retain their own state.

## 9. Non-goals

- Multiple projects in one window (one project per window/session).
- Reloading the whole process into a new cwd.
- Copying global secrets into project files.
- Treating a worktree as a project: a worktree belongs to a project.

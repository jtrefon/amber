# Window worktrees — per-window isolated working copies

Issue: two amber sessions in one directory cannot build, debug or commit
independently — the build is in-tree, `git` has one HEAD and one index, and a
branch switch moves files under the other session. This proposal gives every
chat window its own git worktree, created and reaped by the harness. (New
issues.md entry to be registered with this proposal.)

Status: proposal — sign-off required before production code (AGENTS.md
Red → Proposal → Sign-off → Green).

## Problem

### 1. One directory means one writer

| Conflict | Mechanism |
|---|---|
| Build artifacts | `make` writes `.o`/`.d` next to sources and links `amber`, `amber-cli`, `amber-bench`, `run_tests` into the repo root. Two sessions = two `make`s over the same files; running a binary while the other relinks it fails with `ETXTBSY` |
| Git state | one HEAD, one index. `index.lock` collisions; a branch switch in one window moves files under the other window's in-flight run |
| Branch exclusivity | git already refuses one branch in two worktrees (`fatal: 'main' is already used by worktree at …`) — the property that makes isolation real |
| Bench output | `bench/results/*.json` is written into the tree |
| Agent state | per project path (`Workspace::state_dir()`, `lib/workspace.cpp:153-163`) — shared, which is what we want; but the regressions above are not |

### 2. There is no per-window workspace concept to hang it on

- `Workspace::root()` is a process-global, cached-once value
  (`lib/workspace.cpp:29-52`) used for **both** confinement and state.
- `Window` has no cwd/root field (`tui/window.h:21-66`); `agent::Config` has no
  workspace field (`include/agent/config.h`). Tools resolve the static directly:
  `tools/read_tool.cpp:26`, `tools/write_tool.cpp:30`,
  `tools/search_tool.cpp:41-44`, `tools/bash_tool.cpp:178,212,293`,
  `tools/process_tool.cpp:97-98`, `lib/job.cpp:239`.
- The UI-side git reader uses the **process cwd**, not the root
  (`tui/render_engine.cpp:745-786`).
- `tui/window_manager.h:41-46` already anticipates the fix: the skill/memory
  pool is "keyed by workspace root so a future per-window project switch
  resolves a different pair with no further plumbing".

### 3. Nothing reaps what a crashed session leaves behind

No lock file, pid file or lease exists anywhere in the state tree; there is no
reaper for sessions, logs or bench workspaces. A worktree registry must be the
first component with an owner-liveness marker, or a second amber instance can
"reap" a live window's workspace.

## Decision: core, not a plugin

- No window/session lifecycle event exists (`include/agent/event_bus.h:15-32`);
  no capability kind covers it (`include/agent/plugin_capability.h:29-40`);
  window-level extension points are deferred (`docs/spec/plugins/plugin-framework.md:231-233`).
  A plugin cannot learn that a window was created.
- The framework's own rule (developer guide :41-43): a diff that must touch the
  TUI is a framework gap to declare, not a plugin to write.
- Skills are prompt text only (`tools/skill_tools.cpp:125-162`): the git
  library documents the convention, the core executes it.
- The agent-facing half is plugin-shaped and follows later: a `worktree` tool in
  the coding domain plugin plus a library entry.

## Design

### Two roots (load-bearing change)

| Root | Meaning | Keys |
|---|---|---|
| `Workspace::project_root()` | where amber was started (cwd / `AMBER_WORKSPACE`) | `state_dir()`, `local_dir()`, settings, policy, caches, skill/memory pools |
| `Workspace::workspace_root()` | the active window's worktree, else project root | `confine()`, `relative()`, bash cwd, job cwd, search root, shell classification |

Plumbing rides `RunScope` (`include/agent/run_scope.h:14-18`), the existing
per-run thread-local chain already carrying the cancel token and skill catalog;
it is captured and re-installed across the tool-dispatch `std::async` hop, so
worker threads resolve the same root. `RunScope.leaf` gains
`const std::string* workspace_root`; the `Agent` holds its own root and installs
it at both scope sites (`lib/agent.cpp:1125`, `:589`). No scope (CLI, bench,
tests) → project root, unchanged.

UI-side call sites take the active window's root explicitly:
`tui/render_engine.cpp:745-786` (`git -C <root>`), `@file` expansion
(`tui/tui_input.cpp`), `pick_reference_file` (`tui/tui.cpp:924`),
`run_command_async` (`tui/tui.cpp:326`).

### WorktreeService (lib, pure, unit-testable)

`include/agent/worktree.h` + `lib/worktree.cpp`:

- `detect(root)` → `{is_repo, head_oid}` via `git -C <root>` with fork/exec argv
  (never shell interpolation; precedent `lib/environment.cpp:38-62`).
- `create(name, base_oid)` → `git worktree add --detach <path> <base>` in
  `$TMPDIR/amber-wt/<project-slug>/<name>` (tmpfs here, volatile by design).
- `remove_safe(path)` → refuses when dirty (git's own check) **or** when
  `git for-each-ref --contains <head_oid>` is empty (detached commits reachable
  from no ref are at risk of expiry — the price of detached HEAD).
- `reap(entries, live)` → safe-removes orphans; a vanished path runs
  `git worktree prune` (clears stale `.git/worktrees/<name>` metadata) and drops
  the entry.
- Registry `<state>/worktrees.json`, atomic tmp+rename (`lib/session.cpp:266-284`
  pattern): `{name, path, base_oid, head_oid, session_id, owner{pid,
  start_ticks}, created_ms}`. Owner liveness = pid + `/proc/<pid>/stat` start
  ticks, so a live second instance's worktrees are never touched.

### Lifecycle

| Event | Behavior |
|---|---|
| `/window new [name]`, Ctrl+N | not a repo or `worktree.auto=off` → plain window, silently. Else create, set the window's root + title, push an info line, register, `save_workspace_now()` immediately |
| `/window new noworkdir [name]` | skip — a `completions.json` node, never a C++ string test (AGENTS.md hard rule) |
| Window close | attempt safe removal; refused → keep + notify why (branch-less commits expire in ~90 days, so "kept" is loud) |
| Restore | window entries persist `{root, name/path, base_oid, head_oid}`; path exists → re-attach; `/tmp` wiped → recreate at `head_oid` (fallback `base_oid`); impossible → project root + notify |
| Startup | reap entries whose owner is dead and which no restored window references; report kept ones |
| Crash | create/close save eagerly, so the SIGKILL window is milliseconds; dead owners are reaped next start |

### Command surface

`completions.json` gains `window.new.children.noworkdir`; `worktree.auto on|off`
(default on) is a normal setting; `/window list` shows each window's workspace.
Optional: `/window worktree list` as a feed over the registry so kept orphans
are visible.

## Sequence (each PR red → green)

1. **PR1** — Workspace split, zero behavior change: `lib/workspace.{h,cpp}`,
   `include/agent/run_scope.h`, call-site classification, tests for scope
   override / confinement / state keying.
2. **PR2** — `WorktreeService` + registry + reap, temp-repo fixture tests. No TUI.
3. **PR3** — TUI integration: tree node, setting, `Window.root` + persistence
   (`WorkspaceState::WindowEntry`, version bump, backward compatible),
   create/close/restore/reap hooks, status-bar `git -C`, notification.
4. **PR4** — coding plugin `worktree` tool + `git-worktrees.md` library entry.

## Invariants

1. Never `git worktree remove --force` automatically.
2. Never remove a worktree whose HEAD is unreachable from any ref.
3. Never touch a worktree whose owner process is alive.
4. Only registry-owned paths are candidates; user-made worktrees are invisible.
5. Creation failure never blocks window creation — fall back + notify.
6. Names validated, directories `0700`, argv exec only.
7. Amber never creates or deletes branches.

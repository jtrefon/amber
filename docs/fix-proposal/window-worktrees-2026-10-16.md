# Window worktrees — per-window isolated working copies

Issue: two amber sessions in one directory cannot build, debug or commit
independently — the build is in-tree, `git` has one HEAD and one index, and a
branch switch moves files under the other session. This proposal gives every
chat window its own git worktree, created and reaped by the harness. (New
issues.md entry to be registered with this proposal.)

Status: implemented (PR1–PR3). The 2026-10-21 follow-up below adds the
lifecycle the first cut was missing: a working `/set worktree off`, a leave
flow, and a full JSON-driven workspace manager.

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

## Observed, 2026-10-16

Two agent sessions in one checkout produced exactly this failure while the
feature was being built: the second session's commit landed on whichever branch
the first had checked out, a red test reached an unrelated feature branch, and
its PR went red on all three compilers before the branch had to be cleaned by
hand. Nothing was lost; the cost was the cleanup — and that is the cost this
proposal removes by giving each window its own working copy.

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
The manager below replaces the optional registry readout.

## Lifecycle follow-up (2026-10-21)

The first cut left `/set worktree off` flipping a config bool while the window
stayed attached, and a restart re-attached the recorded root regardless of the
setting. There was also no supported way to leave, keep, or re-attach a
workspace. The fix:

### States and ownership

- **live** — claimed by a window (registry `session_id`; before the first save,
  a stable `window-<id>` claim).
- **parked** — released, kept on disk, never reaped. `/worktree detach` parks,
  and a restore with `worktree off` parks; `/worktree attach <name>` claims
  again.
- **kept** — unclaimed and unsafe to remove (dirty or a HEAD no ref reaches).
  Reported, never force-removed.
- Exactly one live window per workspace. `claim_worktree` refuses a claim held
  by another live session; a stale claim is reclaimable (the registry is a
  lease, not an identity).

### Leave flow (no data loss, no force)

`/set worktree off` and `/worktree main` go through the explicit leave flow.
Window close uses the same safe removal primitive (and parks what it cannot
remove) but has no dialog: the window is already going away.

1. Refuse while the window's agent is running.
2. Dirty tree → `[Cancel]` or `[Stash and leave]` (`git stash push
   --include-untracked -m "amber: leave <name>"`).
3. Detached HEAD reachable from no ref → `[Cancel]` or `[Create branch
   amber-leave-<name>-<oid8> and leave]` (the only branch amber ever creates,
   and only on this explicit choice).
4. Clean and reachable → `[Cancel]` or `[Leave and remove]`.
5. On success the window's root returns to the project root, its conversation
   and scrollback are untouched, and sessions plus the workspace file are saved
   atomically. Cancelling changes nothing.

A restored window whose recorded workspace exists re-claims it. With
`worktree off`, it parks the workspace instead and restores at the project
root, with a notice naming the attach command. Other windows are unaffected:
the setting is the default for new windows, and each existing window leaves
individually.

### JSON command surface (no hardcoded names)

`completions.json` documents the `worktree` verb tree: `new [name] [at <ref>]`,
`attach`, `detach`, `remove`, `status`, `list`, `prune`, `main`. The name lists
under `attach` and `remove` are a **registry feed**
(`FeedManager::refresh_worktree_feed()`), each leaf carrying a host-derived
action (`core.worktree.attach.<name>`) and a help line with state/branch/owner.
Handlers are pure `(action, arg)` closures; there is no C++ command path,
choice list, or usage string.

### Manager behavior

| Verb | Behavior |
|---|---|
| `new [name] [at <ref>]` | Park the active window's current workspace, resolve the ref to a commit, create a detached worktree, register and claim it |
| `attach <name>` | Refuse a live claim by another window; park the current workspace; claim the named one |
| `detach` | Park the current workspace and return to the project root |
| `remove <name>` | Only an unclaimed workspace; safe removal (dirty/unreachable refused) |
| `status` | Path, branch/HEAD, clean/dirty, reachability, state, owner |
| `list` | Every registry entry with state, branch, path and the reason a kept one is unsafe |
| `prune` | Drop entries whose directory vanished (and git's stale metadata); parked existing workspaces survive |
| `main` | Leave via the safe flow above |

### Additional invariants

8. A skipped claim (another live window) is never overridden.
9. `worktree off` is honored on restore, not just on creation.
10. Parked workspaces survive every reap pass; only an explicit `remove`
    deletes them.
11. The attach/remove names are a JSON feed, never a C++ list.

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

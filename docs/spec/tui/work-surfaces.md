# Spec: Work surfaces for jobs, subagents, watches and sessions

- **Status:** Implemented and shipped in v0.6.0 (2026-10-22): job/subagent
  metadata, status cells, `/work` (live timed modal with tabs/filter/selection
  preservation), and resource viewers. Project sessions are implemented in
  their own spec.
- **Contract:** Build contract for 0.6.0. Decisions locked 2026-10-22.
- **Tracking:** #320; metadata #326; board #327; viewers #328; release #331.
- **Related:** `command-namespace.md`, `command-shortcuts.md`,
  `plugins/watch-observer.md`, `docs/spec/plugins/plugin-framework.md`.

## 1. Resource model

One vocabulary, four resource kinds:

| Kind | Lifetime | Owner | Can attach? | Lifecycle verb |
|---|---|---|---|---|
| `session` | conversation/window | host | already a window | `close` (window), `save`, `fork` |
| `job` | OS process group | originating window | yes, read-only viewer | `stop` |
| `agent` | one `task` tool call (subagent) | parent turn/window | yes, live viewer | `stop` via parent cancel |
| `watch` | plugin worker | owner window | yes, read-only viewer | `stop` |

A resource is not a chat session. A resource viewer is a window kind, not a fake
`Agent` and not a saved session.

Resource identity:

- session/window: stable `owner_id` (persisted) plus current window id;
- job: `JobInfo.id` plus new `owner_id`/`label`;
- agent: new task record id (`task_<n>`), owner, prompt head;
- watch: existing `WatchRecord.id`/`owner_id`.

## 2. Status indicators

Four compact cells, left of the model cell, after `[s…]` root:

| Cell | Meaning | Tone |
|---|---|---|
| `[s2/3]` | active window index / open windows | `Banner` (white-on-blue) |
| `[j2]` | running jobs | `Warn` (yellow-on-blue) |
| `[a1/2]` | active subagents / configured max | `Good` (green-on-blue) |
| `[w2!]` | running watches, `!` attention | `Dim` (cyan-on-blue) |

Rules:

- A `StatusText` has exactly one tone, so these are four separate segments.
- `activity` no longer prints “N jobs”; it keeps the running-tool/wave readout.
- `[a…]` is hidden when the `task` tool is unavailable or subagents are disabled.
- `[j1/2]` is shown only while a job viewer is focused: focused job / running
  jobs. With no viewer it is `[j2]`; jobs have no configured maximum.
- `[s…]` counts **open windows**, not saved sessions. Saved sessions stay in
  `/session list` and the browser.
- `[w…]` reuses the watch plugin’s counts and attention flag.
- The cells are a legend for `/work` and the resource roots; command shortcuts
  may differ where collision resolution requires it.

## 3. Work board

A host-owned modal, not a plugin panel in v1.

Why host-owned: `PanelSpec` exposes only `lines(width)` and `on_key(key)`; it
does not pass the selected row, cannot run host actions, and `PanelView` is a
blocking modal with no live tick. The session browser is the working pattern:
a pure model plus a host loop. We will reuse that pattern.

Target:

- Open with `/work` and a key (`Alt+J` recommended; `/work` is the guaranteed
  entry point).
- Sections/tabs: Sessions, Jobs, Agents, Watches. Tab cycles sections; `/`
  filters; Up/Down/PgUp/PgDn navigate.
- Row fields: kind/id, label or title, state, elapsed, owner/window, attention,
  last activity/headline.
- Actions:
  - `Enter` focus/switch a session; open a viewer for a job/agent/watch. The
    board refreshes from live snapshots on a timed tick.
  - `a` attach/open a viewer.
  - `d` detach/close a viewer; resource continues.
  - `x`/`Del` stop the focused resource (confirm; resource-specific).
  - `n` new session or project session.
  - `r` read output/report (full detail in the viewer).
  - `Esc` close board.
- Live refresh: the board is a host modal; it must have a timed tick or
  event-drain loop so it does not freeze while jobs/agents change. This is a
  deliberate difference from `PanelView`.
- No destructive action without confirmation. “Close” and “stop” are distinct
  verbs in the footer.

The board model is pure and unit-tested like `SessionBrowserCore`; painting and
host actions live in `tui/`.

## 4. Resource viewer windows

Add `WindowKind::Chat | Resource` to the window model.

A resource window:

- has a stable `owner_id` for routing, but no `Agent`, no session file, no prompt
  input, no autosave;
- displays a header (kind, id, owner, label, state, elapsed, exit/result) and a
  live body (job output tail, agent progress/result, watch journal/report);
- is created by `/window attach <kind> <id>` or the Work board `Enter`/`a`;
- is switched and closed with the existing window keys/commands;
- `close`/detach does **not** stop the resource;
- `x`/`/job stop` stops a job; `/agent stop` cancels the parent task;
  `/watch stop` (or `/plugin watch stop` after migration) stops a watch;
- one resource per viewer in v1; the Work board is the multi-resource overview.

`WindowManager`, session restore, `RunRegistry`, and session autosave must skip
resource windows. A resource window is not part of `workspace.json` sessions.

## 5. Background metadata prerequisites

### Jobs

- Add `owner_id` and `label` to `JobInfo`/`Job`.
- `ProcessStartTool` captures `current_run_scope()->leaf.owner_id` and the
  scoped worktree, exactly as the watch tool does.
- Host-started jobs (`/system`, `/job start`, `/work`) pass the active window’s
  `owner_id` explicitly.
- `JobService` remains the source of truth; the board polls `list()` on its
  tick. No completion event is required for v1, but a `JobCompletedEvent` is a
  later optional enhancement.
- Jobs survive window close by default. The board reports `owner detached`.

### Subagents

The current executor is intentionally opaque: no ids, no state, no active count,
and only a cumulative `launched()` counter. Add:

- a per-task record: id, owner, prompt head, state (`queued|running|done|failed|
  cancelled`), start/elapsed, slot index, current tool, result summary;
- `active()`, `waiting()`, `max()`, `parallel()`, and a snapshot list;
- an executor-level progress observer forwarding `on_tool_call`,
  `on_tool_result`, `on_state` and a bounded output tail from the subagent.
  The existing `subagent_hooks_do_not_leak` contract must be restated as “does
  not leak into the parent agent’s hooks,” because the host board is a new,
  deliberate observer.
- `SubAgentExecutor` remains the slot/queue authority. The viewer/board never
  spawns an agent and never bypasses the disabled/parallel/max policy.
- An agent record is live only for the duration of the `task` tool call; the
  viewer shows the final result after the call returns.

### Watches

- Reuse `WatchRecord.owner_id`, counts and reports.
- The board may query the watch plugin through a host-owned provider or a
  generic `WorkProvider`; it does not read the plugin’s files directly.
- `/watch stop`/the viewer stop action maps to the existing cancellation path.

## 6. Lifecycle matrix

| Event | Job | Agent | Watch | Session/viewer |
|---|---|---|---|---|
| Viewer close/detach | continues | continues until parent returns | continues | viewer gone |
| Chat window close | continues; owner detached | turn-bound; close is rejected while busy | owner-bound: cancel/abandon | session closes |
| `stop` | process group killed | parent task cancelled | poll/worker cancelled | n/a |
| Parent turn cancel | job continues | cooperative cancel | continues | run cancelled |
| App exit | `JobService` shutdown | worker unwind | plugin shutdown | sessions saved |
| Restart | job disappears; no resume | task did not survive | watch marked unknown | sessions restore |

No action silently kills work by closing its view.

## 7. Tests

- **WS-01** status cells render `[s2/3] [j2] [a1/2] [w2!]` with four tones;
  `activity` no longer contains “N jobs”.
- **WS-02** job owner/label is captured from the run scope; host-started jobs
  get the active window owner.
- **WS-03** subagent records show active/max/waiting and update while a task
  runs; the parent sees only the final tool result.
- **WS-04** work board lists all four kinds, filters, selects and performs
  confirm/stop/attach.
- **WS-05** viewer close detaches, resource continues; stop kills/cancels.
- **WS-06** chat close leaves jobs running, cancels owner-bound watches, and
  rejects closing a busy window.
- **WS-07** PTY: board open/Enter viewer/detach/stop; status updates; no focus
  steal.
- **WS-08** no resource window is saved/restored as a session or emits a fake
  agent event.

## 8. Non-goals

- A full background-agent supervisor (the #317 inbox/mid-turn injection).
- Multiple resources in one viewer window in v1.
- Replacing the watch panel/commands before the namespace migration.
- A plugin-owned Work board; panels are too limited in v1. The host board can
  later be exposed through a richer panel contract.

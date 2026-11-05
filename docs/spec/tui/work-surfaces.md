# Spec: Work surfaces for background tasks and sessions

- **Status:** 0.8.0. `TaskService` is the one background-work inventory; `/tasks`
  is its command surface, the Work board shows the same rows, and the status bar
  has one task segment. The earlier job/agent/watch resource table is superseded
  by #377 (consolidation) and #380 (this surface).
- **Contract:** one identity, one state machine, one output path.
- **Related:** `command-namespace.md`, `command-shortcuts.md`,
  `plugins/watch-observer.md`, `docs/spec/agent-loop/background-tasks.md`.

## 1. Resource model

Two things can appear in the Work board:

| Kind | Lifetime | Owner | Can attach? | Lifecycle verb |
|---|---|---|---|---|
| `session` | conversation/window | host | already a window | `close`, `save`, `fork` |
| `task` | command/watch/worker backend | originating window | yes, read-only viewer | `cancel` |

A task is one `TaskRecord`: `id`, `kind`, `state`, `label`, `owner`, times,
progress and a bounded result. `backend_id` maps it to the command, watch or
worker handle; the backend keeps the detail it needs and never keeps a second
list of what exists.

- command: `JobService` job, process group, stdout/stderr pipe;
- watch: `WatchService` worker, journal and report;
- worker: the subagent task record.

## 2. Status indicators

Three compact cells, left of the model cell, after `[s…]`:

| Cell | Meaning | Tone |
|---|---|---|
| `[s2/3]` | active chat position / open chat windows | `Banner` |
| `[t2]` | non-terminal tasks owned by the active window | `Warn` |
| `[t2!1]` | running tasks plus one completed-undelivered task | `Warn` |
| `[a1/2]` | active subagents / configured max | `Good` |

Rules:

- One `tasks` segment replaces the old `[j…]` and `[w…]` counters; a watch is a
  task, so a second watch counter would be two numbers for one set.
- The undelivered count comes from the owner's `TaskInbox` (`pending`), not from
  a UI-local copy.
- `[a…]` is hidden when the `task` tool is unavailable or subagents are disabled.
- `[s…]` counts chat windows only; resource viewers never inflate it.

## 3. Work board

- Open with `/work`; sections are **Sessions** and **Tasks** (Tab cycles).
- A task row is `format_task_row`: id, kind, state, elapsed, label, progress.
  `/tasks list` and the board use the same function for the active window.
- Actions:
  - `Enter` focus/switch a session; open the task viewer.
  - `a` attach/open a viewer.
  - `x`/`Del` cancel the selected task, with confirmation.
  - `n` new session; `/` filter; Up/Down/PgUp/PgDn navigate; `Esc` close.
- Live refresh: the board reads `TaskService::all()`/`generation()` on its timed
  tick; it never freezes while a task changes.
- No destructive action without confirmation. The viewer's close/detach is not
  a cancel.

## 4. Resource viewer windows

A resource window is a `WindowKind::Resource`, not an Agent and not a saved
session.

- `task` is the background-work viewer: `format_task_detail` header plus the
  bounded backend output (command) or watch report, capped at 64 KiB.
- Created by `/tasks output <id>` or `/window attach task <id>`.
- Closing/detaching keeps the task running; `/tasks cancel <id>` (or `x` on the
  board) is the explicit stop.
- Closing a chat window cancels its non-terminal tasks and drops pending
  delivery; a cancelled task never wakes the window that just closed.
- Resource windows are excluded from sessions, autosave and run slots.

## 5. Background metadata

- `TaskService` owns the record, owner scope, state machine, caps, completion
  event and wait.
- `ProcessStartTool`, a `bash` timeout handoff and `watch start` all register a
  task before the backend can finish; the completion sink calls
  `TaskService::finish` outside every lock.
- `/tasks start` is the one host-initiated command spawn; it uses the same
  `start_command_task` path as `process_start`.
- The owner is captured from the run scope for tools and from the active window
  for `/tasks start`; `label` and `notify` travel on the record.
- `JobService` and `WatchService` remain execution backends; neither is an
  inventory.

## 6. Lifecycle matrix

| Event | Task | Session/viewer |
|---|---|---|
| Viewer close/detach | continues | viewer gone |
| Chat window close | non-terminal owner tasks cancel; delivery dropped | session closes |
| `cancel` | process group killed / watch asked to stop | n/a |
| Parent turn cancel | task continues | run cancelled |
| App exit | backend shutdown | sessions saved |
| Restart | no resume; a dead task is unknown | sessions restore |

No action silently kills work by closing its view.

## 7. Tests

- **WS-01** status renders `[s2/3] [t2] [a1/2]`; `[t2!1]` carries undelivered;
  no `jobs` or `watch` counter remains.
- **WS-02** task owner/label is captured from the run scope; `/tasks start`
  captures the active window.
- **WS-03** subagent records show active/max/waiting and update while a task
  runs; the parent sees only the final tool result.
- **WS-04** the Work board lists sessions and tasks, filters, selects and
  performs confirm/cancel/attach; its task rows match `/tasks list`.
- **WS-05** viewer close detaches; `/tasks cancel` kills/cancels; a finished
  task is an error, not a state change.
- **WS-06** chat close cancels owner-bound non-terminal tasks and drops pending
  delivery; a busy window is rejected.
- **WS-07** PTY: `/tasks start`, `/job list` alias, task viewer, confirm/cancel,
  board open/close; no focus steal.
- **WS-08** no resource window is saved/restored as a session or emits a fake
  agent event.
- **WS-09** `work.auto_attach` still opens viewers for newly running jobs and
  sub-agents without moving focus; the task inventory is the status/board
  source, not a second wake path.

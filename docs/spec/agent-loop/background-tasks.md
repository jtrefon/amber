# Background tasks — one inventory, one state machine

- **Status:** contract for 0.8. `TaskService` is the registry; the command,
  watch and worker engines remain execution backends.
- **Epic:** #366. Implementation: #375 (this contract), #376 (delivery),
  #377 (consolidation), #378 (persistence), #379 (workers), #380 (`/tasks`).
- **Scope:** core (`lib/` + `include/agent/`). No UI types cross this boundary.

## Why one registry

Amber already runs three kinds of work with three identities:

| Kind | Engine | Identity today | Gap |
|---|---|---|---|
| command | `JobService` | monotonic counter, host-global | no owner, label, completion or inventory |
| watch | `WatchService` | watch id, owner, journal | own store and report; a second inventory |
| worker | `SubAgentExecutor` | none (synchronous) | no task id, no detached lifetime |

A second queue beside the watch report path is the failure this contract
prevents. There is **one** inventory (`TaskService`), **one** owner model, and
**one** state machine. A backend may keep the detail it needs (a watch journal,
a job's output buffer); it does not keep a second list of what exists.

## Record

The record is a value: IDs are stable, the map owns the copy, and consumers get
snapshots. `TaskService` does not execute anything.

```cpp
enum class TaskKind : std::uint8_t { Command, Watch, Worker };
enum class TaskState : std::uint8_t {
    Queued, Running, Succeeded, Failed, Cancelled, TimedOut, Interrupted, Unknown
};
enum class TaskNotify : std::uint8_t { Auto, Digest, Silent };
```

| Field | Meaning |
|---|---|
| `id` | `task_<base32(ms)>_<base32(random)>`; generated once, never reused |
| `kind` | which engine owns execution |
| `state` | the state machine below |
| `notify` | delivery policy: `Auto` wakes at a safe boundary, `Digest` batches, `Silent` never wakes |
| `label` | short human string, bounded |
| `owner` | `owner_id` (window/session), `project_root`, `worktree`, captured at spawn |
| `command` / `goal` | command or watch line; worker goal; empty where not applicable |
| `cwd` | confined working directory at spawn |
| `created_ms` / `started_ms` / `ended_ms` | wall clock, `0` until the step happens |
| `progress` | one line: last tool, line or percent; never a token stream |
| `result` | exit code, signal, bounded summary, bounded output tail, artifact path |
| `error` / `cancel_reason` | the typed reason, never a guessed one |
| `delivered_at_ms` | `0` until the inbox accepted the completion |

Bounded fields are capped by the service; a backend cannot grow a record without
bound.

## State machine

```
Queued ──► Running ──► Succeeded
  │           │   ├──► Failed
  │           │   ├──► TimedOut
  │           │   ├──► Interrupted
  │           │   └──► Unknown
  ├───────────┴──► Cancelled
  └──────────────► Unknown
```

Rules:

- `Queued -> Running` is the only start transition.
- `Queued | Running -> Cancelled` is the only user cancel.
- A terminal state is immutable: every later `started`/`progress`/`finish`/
  `cancel` returns false and changes nothing.
- `Unknown` is the honest answer when a backend cannot report or cannot cancel.
  It is never a synonym for `Cancelled`.
- Every accepted transition increments one generation counter; a pull UI reads
  the generation, then a snapshot, and never sees a half-written record.

The legal-transition table is data, not a chain of `if`s, and an illegal
transition is a reported error.

## Ownership and cascade

- A task belongs to the window/session that started it, not to the turn that
  spawned it. `owner_id` is the stable identity already carried by
  `Agent::set_owner_id`/`RunScope`.
- `list(owner_id)` is scoped. `all()` exists for the Work board and the host,
  not for a model-facing tool.
- Closing a window cancels its non-terminal tasks and abandons their delivery;
  a cancelled task never wakes the window that just closed.
- A task outlives its spawning turn. It does not outlive its owner.

## Admission

Caps are injected at construction (`TaskLimits`), not read from config in v1.
A cap counts **non-terminal** records only.

| Kind | Per owner | Global |
|---|---:|---:|
| command | 20 | 64 |
| watch | 20 | 64 |
| worker | 1 | 4 |
| total | 32 | 96 |

Over a cap the create call returns an empty id and an error that ends with
`do not retry`; retrying a full queue cannot succeed and must not become a
spin.

## Safe delivery boundaries

Delivery is #376, but the contract is fixed here because the state machine
must support it:

1. **After a tool-result batch**, before the next model request.
2. **At idle**, before a user prompt starts a new turn.

A completion is enqueued by the owner and acknowledged only when the agent
accepts it into a turn. A busy turn is never interrupted; a user prompt wins
over a completion. The queue is keyed by `owner_id`; no UI type crosses into
the core.

## Events

One typed `EventBus` event per accepted transition, published **after** the
state is committed and **outside** the map lock:

| Event | When |
|---|---|
| `TaskStartedEvent` | `Queued -> Running` |
| `TaskProgressEvent` | progress line replaced |
| `TaskCompletedEvent` | any terminal non-cancel state |
| `TaskCancelledEvent` | `-> Cancelled` |

No per-token events. A backend reports a fact once; a completion callback and
an event for the same fact is a duplicate, not a second opinion.

## Cancellation

- `cancel(id, reason, token)` invokes the backend's cancel handler outside the
  map lock.
- A backend that returns success moves the task to `Cancelled`; a backend that
  cannot stop the work moves it to `Unknown` with the reason recorded.
- Command cancellation kills the **process group**, not only the direct child.
  A token check between polls is not a kill.
- The cancel call is idempotent: a terminal task returns false without changing
  state.

## Restart and persistence (#378)

`TaskService` itself is in-memory; `TaskStore` is the durability boundary. Both
hosts attach one (`TuiHost`, `CliHost`); bench and probe runs attach none, so a
hermetic run writes no task files.

- **Layout.** Records live in `<state>/tasks/<task_id>.json`, one private file
  per task; artifacts (full output, worker transcript, watch journal) live in
  `<cache>/tasks/<task_id>/`. Both suffixes come from `config_paths.cpp`, the
  same functions `/get config paths` prints, so the readout and the writer
  cannot drift.
- **Write path.** Every transition saves the newest record before its event is
  published. The save re-reads the record under the map lock and runs outside
  it, serialized by a separate persistence mutex. The store never writes an
  index: `list()` scans the directory, so no half-written index can exist.
- **Atomicity.** `write_private_file` writes a temp file in the same directory,
  fsyncs and renames; the record is always either the old file or the new one.
  Files are `0600`, directories `0700`.
- **Restart rules.** A `Running` record from a previous process becomes
  `Unknown` (a process cannot be reattached), with `ended_ms` set to its last
  activity and its progress preserved. A `Queued` record becomes `Cancelled`
  with reason `host restarted before start`, because nothing ever ran. Terminal
  states are immutable.
- **Clean shutdown.** The host calls `TaskService::shutdown()` after its UI has
  joined: `Running` becomes `Interrupted`, `Queued` becomes `Cancelled`. A
  crash skips that pass and leaves the record `Running`, which the next start
  turns into `Unknown`.
- **Delivery.** `delivered_at_ms` is part of the record and is set only after
  the inbox ack. The host seeds the inbox from terminal, undelivered records
  after restoring the windows; a delivered record is never emitted again, so a
  crash between publish and ack redelivers rather than loses. A `Silent` task
  owes no message and is marked delivered when it finishes.
- **Retention.** At startup the host prunes: newest 200 records per owner,
  artifact files older than seven days or beyond 256 MB (oldest first). An
  undelivered record and a non-terminal task's artifact are never removed. The
  walk skips symlinks and counts errors instead of failing startup.
- **Corruption.** A malformed or newer-schema record is skipped and counted by
  `TaskStore::skipped()`; startup continues with the rest.
- An id is never reused across a restart; the time+random form makes that a
  property, not a hope.

## Non-goals

- Durable execution or reattaching to a running process after restart.
- A scheduler, a distributed queue, or a second governor.
- Replacing `JobService`, `WatchService` or `SubAgentExecutor`.
- A model-facing `tasks` tool in this contract; that is #377/#380.

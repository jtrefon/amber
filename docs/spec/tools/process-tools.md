# Background commands — `process_start` and `tasks`

`process_start` is the one spawn tool for a shell command. It returns a task id
immediately; the process keeps running across turns. `tasks` is the one
inventory and lifecycle surface for every kind of background work, so a command,
a watch and a worker share one id, one state machine and one output path.

## `process_start`

- **Factory**: `include/agent/tools.h` → `make_process_start_tool(JobService&, TaskService*)`
- **Source**: `tools/process_tool.cpp`
- **Approval**: required (it starts a process group)
- **Read-only**: no

| Field | Meaning |
|---|---|
| `command` | shell command, run through `/bin/sh -c` in its own process group |
| `timeout` | hard lifetime in seconds (default 600, 0 disables) |
| `idle_timeout` | seconds without output before auto-kill (default 30, 0 disables) |
| `cwd` | confined working directory; defaults to the workspace root |
| `label` | short label for `tasks list`; defaults to the command head |
| `notify` | `auto`, `digest` or `silent` |

**Output**: the task id as a bare string. **Meta**: `task_id` and the backend
`job_id`.

The tool creates the task record before it starts the job, so a command that
exits immediately cannot outrun its completion. The job's reader thread invokes
the completion sink once; the sink finishes the task record with the exit state
and exit code. `tasks output` reads the job's bounded output through the backend
id, so the completion path never holds the job service lock.

## `tasks`

- **Factory**: `include/agent/tools.h` → `make_tasks_tool(TaskService&, JobService&, CancellationToken)`
- **Source**: `tools/tasks_tool.cpp`
- **Approval**: `cancel` only
- **Read-only**: no (the tool owns one mutating action)

| Action | Behaviour |
|---|---|
| `list` | the owner's tasks: id, kind, state, elapsed, label, progress |
| `status` | one record: state, elapsed, owner, progress, exit, error, artifact |
| `output` | a bounded tail from `since`; returns `next_since` when more remains |
| `cancel` | asks the backend to stop; a finished task is an error, not a no-op |
| `wait` | condition-variable wait bounded by `timeout_s` and the run token |

`list` uses the run scope's owner id; there is no all-owners action on the tool
surface. `status`, `output`, `cancel` and `wait` reject a task owned by another
window. `output` is capped at 8 KiB per call; a 1 MiB job is paged, never
pasted whole into the conversation.

## Backends

`JobService` remains the command engine. `TaskService` owns the record, the
state machine and the owner scope; `backend_id` maps a task to its job, watch or
worker handle. Cancellation reaches the backend through the handler attached at
creation, so `tasks cancel` kills the command's process group rather than only
recording an intention.

## Invariants

1. A task record is created before its backend can complete.
2. The completion sink publishes outside every lock and calls only
   `TaskService::finish`.
3. Output is bounded by `tasks`; the full log stays in the backend.
4. `cancel` is approval-gated; `list`, `status`, `output` and `wait` are not.
5. A terminal task is immutable; a second finish or cancel is rejected.
6. Owner scoping is checked on every id-taking action, not only `list`.

## Tests

`tests/tasks_tool_test.cpp`: owner scoping, status, bounded paging with `since`,
process-group cancellation, unknown id, bounded wait, cancellable wait, and
`process_start` registering its task. `tests/task_service_test.cpp` covers the
state machine and the completion edge.

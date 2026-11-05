## tasks

One inventory for background work. Commands started with `process_start`, a
`bash` timeout handoff and a `watch start` all appear here with a stable task id.

### Actions

- `action=list` — this window's tasks: id, kind, state, elapsed, label and the
  last progress line.
- `action=status id=...` — one record: state, elapsed, owner, progress, exit,
  error and artifact path.
- `action=output id=... [since=...]` — a bounded tail of the output, starting at
  the byte offset `since`. The response names the next offset when more remains,
  so a long log is paged rather than pasted into the conversation.
- `action=cancel id=...` — stop the task. Commands lose their process group;
  watches and workers are asked to stop. Cancelling a finished task is a clear
  error, not a state change.
- `action=wait id=... [timeout_s=...]` — a bounded, cancellable wait. It returns
  the final record, or says the task is still running after the bound.

### When to use it

`process_start` returns a task id and the process keeps running across turns;
`tasks output` follows it without blocking, and `tasks wait` is for the moment
the next step genuinely needs the result. A `bash` command that goes inactive
hands back a task id for the same inventory. A watch reports through the same
record, so one list answers "what is running?" for every kind.

`list`, `status`, `output` and `wait` are read-only. `cancel` changes state and
asks for approval.

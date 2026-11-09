## delegate

Launch a detached worker with fresh context. The call returns a task id
immediately; the worker runs in the background, and its bounded report arrives
when it finishes.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `goal` | string | yes | What the worker must do and verify |
| `context` | string | yes | Everything the worker needs: files, paths, constraints. The worker does not see this conversation |
| `expect` | string | yes | The report shape and success criteria |
| `label` | string | no | Short human label for `/tasks` |
| `max_turns` | integer | no | Bound on the worker's tool loop |

**Content**: the task id as a bare string.
**Meta**: `{"task_id": "<id>"}`

A worker shares this session's tools, mode, policy and workspace. It cannot
widen policy, cannot approve its own tools, and cannot delegate again. Only the
final report enters this conversation; the full transcript is the task's
artifact (`tasks output <id>` reads it).

Use `tasks status <id>` for the record, `tasks wait <id>` when the next step
needs the result, and `tasks cancel <id>` to stop it. A worker queues behind the
current turn and behind earlier delegates when the host is serial (the default).

Delegate a self-contained survey, a research question, or an isolated edit. Keep
work that needs this conversation's context here.

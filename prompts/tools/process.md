## process_start

Launch a command in the background. Returns immediately with a task id.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `command` | string | yes | — | Shell command to run |
| `timeout` | integer | no | 600 | Hard lifetime in seconds (0 = no limit) |
| `idle_timeout` | integer | no | 30 | Seconds idle before auto-kill |
| `cwd` | string | no | workspace root | Working directory |
| `label` | string | no | command head | Short human label for task lists |
| `notify` | string | no | `auto` | `auto`, `digest` or `silent` |

**Content**: the task id as a bare string.
**Meta**: `{"task_id": "<id>", "job_id": "<id>"}`

The process keeps running across turns. Use `tasks output <id>` to follow it,
`tasks status <id>` for the record, `tasks wait <id>` when the next step needs
the result, and `tasks cancel <id>` to stop the process group.

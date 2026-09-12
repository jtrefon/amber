## process_start

Launch a command in the background. Returns immediately with a `job_id`.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `command` | string | yes | — | Shell command to run |
| `timeout` | integer | no | 600 | Hard lifetime in seconds (0 = no limit) |
| `idle_timeout` | integer | no | 30 | Seconds idle before auto-kill |
| `cwd` | string | no | workspace root | Working directory |

**Content**: bare `job_id` string — pass this to `process_read` / `process_stop`.
**Meta**: `{"job_id": "<id>"}`

## process_read

Fetch new output from a background job since the last read.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `id` | string | yes | — | Job id from `process_start` |
| `all` | boolean | no | false | Return full captured output instead of delta |

**Content**: status line + delta output.
**Meta**: `{"job_id": "<id>", "state": <int>, "delta": <true|false>}`

## process_stop

Terminate a background job and return its captured output.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `id` | string | yes | — | Job id from `process_start` |

**Content**: "stopped" notice + captured output.
**Meta**: `{"job_id": "<id>"}`

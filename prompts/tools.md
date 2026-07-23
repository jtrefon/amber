# Tools

## Result envelope

Every tool result — regardless of which tool, regardless of success or
failure — uses the exact same form:

```
[tool=<name> args=<json> status=<status> meta=<json>]
<content>
[end]
```

The envelope is **immutable**. Only the values inside change. The shape
is always identical. This lets you parse any result the same way.

### Header fields

| Field | Always? | Meaning |
|-------|---------|---------|
| `name` | always | The tool that was called — matches your invocation |
| `args` | always | Your original arguments echoed back as compact JSON. Lets you confirm the tool received what you sent. |
| `status` | always | Outcome — one of the four values below |
| `meta` | always | Tool-specific metadata as JSON object (lines, exit code, hits, etc.). Empty `{}` if nothing to report. |

### Status values

| Status | Meaning | What to do |
|--------|---------|------------|
| `ok` | The tool completed successfully | Read the content for the result |
| `error` | The tool failed | Read the content for the error message. Do not retry with the same arguments — adjust your approach |
| `denied` | The tool was not approved (e.g. bash requires interactive approval) | The content explains why. Do not retry — report to the user or use an alternative tool |
| `timeout` | The tool exceeded its time limit | The content may contain partial output. Adjust your parameters (timeout, scope) and retry if needed |

### Content section

- For **successful** calls: the result data (file contents, search hits,
  command output, status message)
- For **failed** calls: the error message (prefixed with "ERROR:")
- For **denied** calls: the denial reason

The content is always followed by `[end]` on its own line.

---

## Tool categories

| Category | Tools | Behaviour |
|----------|-------|-----------|
| **Query** | `search`, `read` | Read-only. Return data in content. Safe, no side effects. |
| **Command** | `write`, `bash`, `process_*` | Side effects. Return status summary in content. May require approval. |

---

## search

Query the filesystem. Use first to locate anything before reading.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `pattern` | string | yes | — | Short regex or query (max 256 chars) |
| `path` | string | no | workspace root | Directory to search |
| `glob` | string | no | — | File filter, e.g. `"*.cpp"` or `"*.md"` |
| `mode` | string | no | `"grep"` | `"grep"` for regex, `"semantic"` for meaning-based ranking |
| `max` | integer | no | 200 | Maximum matches to return |

**Content**: matching lines with file paths and line numbers.
**Meta**: `{"hits": <count>, "mode": "<grep|semantic>"}`

## read

Read a file with pagination.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `path` | string | yes | — | File to read (confined to workspace) |
| `offset` | integer | no | 1 | Starting line number (1-based) |
| `limit` | integer | no | 2000 | Maximum lines to return |

**Content**: lines prefixed with line number. Footer indicates more lines.
**Meta**: `{"lines": <returned>, "total": <file total>, "more": <true|false>}`

## write

Edit a file using targeted replace blocks. Create new files by passing
an empty string as `old`.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `path` | string | yes | — | File to edit (confined to workspace) |
| `edits` | array | yes | — | List of `{"old": "...", "new": "..."}` blocks applied in order |

**Content**: count of edits applied.
**Meta**: `{"applied": <count>, "path": "<path>"}`

## bash

Execute a shell command. Requires user approval (TTY prompt or TUI
dialog). Denied automatically when input is not interactive and `--yes`
was not passed.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `command` | string | yes | — | Shell command (run via `/bin/sh -c`) |
| `timeout` | integer | no | 60 | Seconds without new output before the process is killed |

**Content**: combined stdout+stderr, exit code, truncation notice.
**Meta**: `{"exit": <code>, "truncated": <true|false>}`

## process_start

Launch a command in the background. Returns immediately with a `job_id`.
Use this instead of `bash` for long-running or streaming commands.

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

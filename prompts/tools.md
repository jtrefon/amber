# Tools

Invoke tools by name with a JSON object of arguments matching the
schema. Results are returned as text and fed back into the conversation.

| Tool | When to use |
|------|-------------|
| `search` | Find symbols, definitions, usages, patterns. Read-only, cheap, broad. |
| `read` | Inspect file contents. Use `offset` to page through long files. |
| `write` | Edit existing files with targeted `old`/`new` blocks. Use `old=""` for new files. |
| `bash` | Build, test, run git, execute shell commands. Requires approval. |
| `process_start` | Launch long-running commands (servers, watchers, builds) in the background. |
| `process_read` | Check progress of a background job. Returns output delta since last read. |
| `process_stop` | Terminate a background job and return its captured output. |

## search

Search the codebase for a pattern. Default mode is regex (grep); set
`mode="semantic"` for meaning-based ranking over an indexed view.

Parameters:
- `pattern` (string, required) — Short regex or query (max 256 chars).
- `path` (string) — Directory or file to search (default: workspace root).
- `glob` (string) — Optional filter, e.g. "*.cpp".
- `mode` (string) — "grep" (default) or "semantic".
- `max` (integer) — Max matches to return (default 200).

## read

Read a file with pagination. The workspace root confines access.

Parameters:
- `file_path` (string, required) — Path relative to workspace or absolute.
- `offset` (integer) — Starting line number (1-indexed, default 1).
- `limit` (integer) — Max lines to return (default 2000).

## write

Make targeted edits to a file. Create a file by setting `old=""`.
Edits are confined to the workspace root.

Parameters:
- `file_path` (string, required) — Path relative to workspace or absolute.
- `old` (string, required) — Exact text to replace (empty for new files).
- `new` (string, required) — Replacement text.

## bash

Run a shell command inside the workspace root. Returns combined
stdout+stderr and exit code. Approved interactively (TTY prompt or
TUI dialog). Fail-safe: denied when stdin is not a TTY and `--yes`
was not passed.

Parameters:
- `command` (string, required) — Shell command (run via `/bin/sh -c`).
- `timeout` (integer) — Seconds of no output before kill (default 60).

## process_start

Start a command in the background and return a `job_id` immediately.
Use this instead of `bash` for long-running or streaming commands.

Parameters:
- `command` (string, required) — Shell command to run.
- `timeout` (integer) — Hard lifetime in seconds (default 600).
- `idle_timeout` (integer) — Seconds of no output before auto-kill (default 30).
- `cwd` (string) — Working directory (default: workspace root).

## process_read

Fetch new output for a background job since the last read.

Parameters:
- `id` (string, required) — Job id from process_start.
- `all` (boolean) — Return full output instead of delta (default false).

## process_stop

Terminate a background job and return its captured output.

Parameters:
- `id` (string, required) — Job id from process_start.

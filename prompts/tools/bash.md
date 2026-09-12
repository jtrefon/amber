## bash

Execute a shell command. Read-only commands (ls, cat, grep, git status, ...)
run freely. In WRITE mode, benign in-workspace changes (writing a file,
moving files inside the workspace, running a build) also run without
approval. Destructive or system-wide commands (rm, dd, git reset, sudo,
package installs, ...) pause for user approval — the request names the
command kind, and once the user grants it for the session or permanently,
later uses of the same kind run without asking. Writing to a path outside
the workspace asks approval once per target folder. READ mode blocks bash
entirely; YOLO mode skips every approval.

Every call runs in a fresh shell whose working directory is the workspace
root — commands already start there, so a `cd <workspace> &&` prefix is
unnecessary and the directory does not carry over between calls.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `command` | string | yes | — | Shell command (run via `/bin/sh -c`) |
| `timeout` | integer | no | 60 | Seconds without new output before the process is killed |

**Content**: combined stdout+stderr, exit code, truncation notice.
**Meta**: `{"exit": <code>, "truncated": <true|false>}`

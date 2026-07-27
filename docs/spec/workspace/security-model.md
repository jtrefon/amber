## Spec: Security Model

### Purpose
Define the layered security controls that prevent the agent from causing damage
outside the workspace. Three layers: mode-based tool gating, workspace path
confinement, and approval gating for side-effect operations.

### Ownership
- **Source files**: `lib/dispatch.cpp` (approval gates), `lib/workspace.cpp` (confinement), `include/agent/config.h` (`AgentMode`), `lib/process.cpp` (process groups, stdin isolation)
- **Test files**: `tests/run_tests.cpp` — approval, confinement, and mode tests

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Tool calls from LLM, user keystrokes, config mode |
| **Output** | Allowed or denied tool execution, confined file access |
| **Error states** | Denied → `ToolResult{ok=false, meta[denied]=true}` |
| **Invariants** | See below. |

### Invariants

1. Read mode blocks ALL non-read-only tools — no file modifications, no bash.
2. Path confinement is enforced before any file I/O in read and write tools.
3. Bash write commands require approval (Read mode blocks them entirely; Write/Yolo auto-approve).
4. Background processes (`process_start`, `process_stop`) require approval.
5. Fail-safe: no `on_approval` handler → tool denied.
6. Fail-safe: non-TTY CLI with no `--yes` → tool denied.
7. All child processes run in a separate process group — `kill_process_group()` can kill the entire subtree.
8. Child process stdin is redirected to `/dev/null` — prevents terminal stealing.

---

### Scenarios

#### [SM-01] Read mode blocks write tools

- **Given**: `mode = Read`
- **Input**: Model calls `write` tool
- **Expected**: Denied at Gate 1: `"tool write is not available in read mode"`. Envelope: `status=denied`.
- **On failure**: File modified in Read mode.

#### [SM-02] Path confinement prevents escape

- **Given**: Workspace = `/project`
- **Input**: Model calls `read("/etc/passwd")`
- **Expected**: `Workspace::confine()` rejects. `ToolResult{ok=false, error="path escapes workspace root..."}`.
- **On failure**: File outside workspace read.

#### [SM-03] Bash write command requires approval

- **Given**: `requires_approval()` called for `rm -rf /tmp/x`
- **Input**: `is_read_only_shell` returns false
- **Expected**: `requires_approval` returns true. Approval gate fires (or auto-approved in Write/Yolo).
- **On failure**: Write command executes without approval in Read mode (but Gate 1 would block it).

#### [SM-04] Fail-safe: no approval handler

- **Given**: `hooks.on_approval` is null, bash write requested
- **Input**: Approval gate checks `approve_tool()`
- **Expected**: `if (!hooks.on_approval) return false` → denied.
- **On failure**: Tool auto-approved.

#### [SM-05] Fail-safe: non-TTY CLI

- **Given**: stdin piped, no `--yes` flag
- **Input**: Gated tool requested
- **Expected**: CLI approval hook checks `isatty(STDIN_FILENO)` → `false` → `Deny`.
- **On failure**: `--yes` bypass required when piped, but user may not know.

#### [SM-06] Process group isolation

- **Given**: Bash command spawns child processes
- **Input**: `spawn_shell()` via direct path
- **Expected**: Child `setpgid(0,0)` creates new process group. Parent `setpgid(pid, pid)` confirms (race-safe). `kill_process_group()` sends `SIGKILL` to `-pid` — kills all children.
- **On failure**: Zombie child processes survive timeout.

#### [SM-07] Bash read-only commands skip approval

- **Given**: `command = "ls -la"`
- **Input**: `requires_approval()`
- **Expected**: `is_read_only_shell("ls -la")` → true. `requires_approval` returns false.
- **On failure**: Approval dialog for `ls`.

#### [SM-08] Search tool bypasses confinement

- **Given**: Path outside workspace
- **Input**: `search({"pattern": "x", "path": "/etc"})`
- **Expected**: `confine()` fails → falls back to raw `/etc`. Search proceeds. **Known policy gap** — search deliberately escapes.
- **On failure**: Search confined to workspace (inconsistent with design).

---

### Cross-references

- **Depends on**: `agent-loop/mode-system.md`, `workspace/path-confinement.md`, `agent-loop/tool-dispatch.md`
- **Depended on by**: `docs/spec/INDEX.md`
- **Test coverage**: `tests/run_tests.cpp`: approval tests (1183–1208), confinement tests (544–578)

### Known gaps

1. **Approval gate (Gate 3) is dead code** — Conditioned on `mode == Read`, but Read mode blocks non-read-only tools at Gate 1. The `on_approval` hook is never reached through dispatch.
2. **Search tool bypasses confinement** — When `confine()` rejects a path, search silently uses the unconfined path (documented as intentional for read-only operations).
3. **Bash tool has no confinement** — Only the working directory is set to workspace root. The command can access any absolute path (by design — builds need system access).
4. **Signal handler not async-signal-safe** — `save_workspace_now()` does file I/O and string allocation. Could deadlock on signal delivery during malloc/write.

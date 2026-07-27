## Spec: Bash Tool

### Purpose
Execute shell commands in a subprocess with idle-timeout (not wall-clock timeout),
output cap (64 KiB), and approval gating for write commands. The tool supports
two execution paths: direct fork/exec (no JobService) and managed via `JobService`
(background process integration).

### Ownership
- **Source files**: `tools/bash_tool.cpp` (301 lines), `lib/process.cpp` (`spawn_shell`, `run_with_timeout`, `kill_process_group`), `lib/job.cpp` (`Job`, `JobService`)
- **Factory**: `include/agent/tools.h` → `make_bash_tool(JobService*, CancellationToken)`
- **Test files**: `tests/run_tests.cpp` — 6 bash-tool tests (lines 1092–1158), 2 approval tests (lines 1183–1208)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `{"command": <string>, "timeout": <optional int, default 60>}`. Timeout is IDLE timeout (seconds without output), not wall-clock. |
| **Output** | `ToolResult{ok, output, error, meta}`. Exit code in `meta["exit"]`. Truncation flag in `meta["truncated"]`. |
| **Error states** | Missing command, spawn failure, idle timeout, non-zero exit, cancellation. |
| **Invariants** | See below. |
| **Thread safety** | `execute()` blocks calling thread. Cancellation polls `CancellationToken` during execution. Jobs managed by `JobService` are thread-safe. |

### Invariants

1. The command runs in `Workspace::root()` as working directory.
2. The command and all its descendants are in a separate process group — `kill_process_group()` sends `SIGKILL` to the entire tree.
3. Idle timeout resets on every byte of output — a chatty command never times out.
4. Output is capped at 64 KiB (`kMaxOutput = 65536`). Excess is discarded (not added to output).
5. Read-only commands (cat, ls, grep, etc.) skip approval gate. Writes require approval.
6. Cancellation (via `CancellationToken`) during execution sets `timed_out=true` and clears the token.

---

### Scenarios

#### [BT-01] Simple command with exit code 0

- **Given**: A command that exits successfully
- **Input**: `{"command": "echo hello"}`
- **Expected**: `ok=true`. Output contains `"hello\n"`. Meta: `{"exit": 0, "truncated": false}`. Output ends with `"[exit 0]"`.
- **Regression guard**: `bash_tool_runs_and_reports_exit` test.

#### [BT-02] Command with non-zero exit

- **Given**: A command that fails
- **Input**: `{"command": "false"}`
- **Expected**: `ok=false`. `error="command exited with status 1"`. Meta: `{"exit": 1}`. Output ends with `"[exit 1]"`.
- **Regression guard**: `bash_tool_runs_and_reports_exit` test (non-zero case).

#### [BT-03] Missing command argument

- **Input**: `{}` or `{"command": ""}`
- **Expected**: `ok=false`, `error="missing 'command'"`.

#### [BT-04] Idle timeout kills silent command

- **Given**: A command that produces no output for `timeout` seconds
- **Input**: `{"command": "sleep 10", "timeout": 2}`
- **Expected**: After 2 seconds of no output, process group receives `SIGKILL`. `ok=false`. `error="timed out after 2s"`. Meta: `{"exit": -1}`. Output ends with `"[command timed out after 2s and was killed]"`.
- **Regression guard**: `bash_tool_times_out` test.

#### [BT-05] Idle timeout reset on output

- **Given**: A command that produces output continuously
- **Input**: `{"command": "while true; do echo ping; sleep 1; done", "timeout": 2}`
- **Expected**: Command survives indefinitely (new output every second resets idle timer). Test stops it externally after observing it outlives the timeout.
- **Regression guard**: `bash_tool_idle_timeout_keeps_progressing` test.

#### [BT-06] Output truncation at 64 KiB

- **Given**: A command that produces very large output
- **Input**: `{"command": "python3 -c 'print(\"x\"*200000)'"}`
- **Expected**: Output capped at 64 KiB. Meta: `{"truncated": true}`. Output ends with `"[output truncated at 65536 bytes]"`.
- **Regression guard**: `bash_tool_truncates_large_output` test.

#### [BT-07] Cancellation during execution

- **Given**: `cancel_token.request()` called while command is running
- **Input**: `{"command": "sleep 30"}`
- **Expected**: Before command completes, cancel token requested → `BashTool` polls it → sets `timed_out=true` → breaks from poll loop → `cancel_token.clear()`. Output: `"[command timed out after 0s and was killed]"` (0s = no idle time tracked).
- **On failure**: Cancel ignored, command runs to completion.

#### [BT-08] Read-only command auto-approved

- **Given**: `requires_approval()` called for `cat` command
- **Input**: `{"command": "cat foo.txt"}`
- **Expected**: `requires_approval()` → `is_read_only_shell("cat foo.txt")` → returns `true` → `requires_approval` returns `false`. Tool auto-approved even in modes that gate approval.
- **Approval skip list**: `cat`, `ls`, `grep`, `find`, `head`, `tail`, `wc`, `sort`, `uniq`, `which`, `type`, `file`, `stat`, `du`, `df`, `echo`, `printf`, `pwd`, `date`, `whoami`, `id`, `env`, `printenv`, `git status`, `git log`, `git diff`.

#### [BT-09] Write command requires approval

- **Given**: `requires_approval()` called for `rm` command
- **Input**: `{"command": "rm -rf /tmp/x"}`
- **Expected**: `is_read_only_shell` returns `false`. `requires_approval` returns `true`. Approval gate fires (depending on mode).
- **On failure**: Write command executes without approval.

#### [BT-10] Command runs in workspace root

- **Given**: Workspace root = `/project`
- **Input**: `{"command": "pwd"}`
- **Expected**: Output: `/project\n`. Command `cwd` is workspace root.
- **Regression guard**: `bash_tool_runs_in_workspace_root` test.

#### [BT-11] JobService path — same behaviour

- **Given**: `BashTool` constructed with `JobService*`
- **Input**: Any valid command
- **Expected**: Same result as direct-fork path. Job registered in `JobService`, cleaned up after result.
- **Regression guard**: `bash_tool_tracked_by_job_service` test.

#### [BT-12] Kill by signal → exit code 128+N

- **Given**: Command killed by `SIGTERM` or `SIGKILL`
- **Input**: `bash -c 'kill -9 $$'`
- **Expected**: Process reaped with `WIFSIGNALED` → exit code = `128 + SIGKILL(9)` = 137. `ok=false`. Error includes signal info.
- **On failure**: Exit code 0 for killed process.

#### [BT-13] Output envelope format

- **Given**: Successful command with output
- **Input**: `{"command": "echo hello"}`
- **Expected**: Envelope: `[tool=bash args={"command":"echo hello","timeout":60} status=ok meta={"exit":0,"truncated":false}]\nhello\n[exit 0]\n[end]`. Output is part of content; envelope is added by `format_tool_envelope` in dispatch.

---

### Cross-references

- **Depends on**: `tools/process-tools.md` (shared JobService), `agent-loop/tool-dispatch.md` (approval gate), `workspace/security-model.md`
- **Depended on by**: `agent-loop/core-loop.md` (tool use in agent loop)
- **Test coverage**: `tests/run_tests.cpp`: 6 bash tests (lines 1092–1158), 2 approval tests (lines 1183–1208)

### Known gaps

1. **Cancellation token not polled in direct-fork path** — Only the JobService path (lines 243–248) polls `cancel_token_`. The `run_with_timeout()` direct path does not check the token at all.
2. **`format_result()` output cap is duplicate** — The output is capped during read (`drain_output` stops at 64K) AND again in `format_result` (truncates if `>= 64K`). Harmless but redundant.
3. **`is_read_only_shell()` uses prefix match** — `catnap` would match `cat` prefix, though the following-char check (`space or NUL`) correctly rejects it. Edge-case safe but fragile.
4. **No workspace path confinement on command** — Bash runs in workspace `cwd` but the command itself can access any absolute path. By design (shell needs full system access for builds, etc.).

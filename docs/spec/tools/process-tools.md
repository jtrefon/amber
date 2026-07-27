## Spec: Process Tools (Background Jobs)

### Purpose
Start, read, and stop long-running background processes that outlive individual
LLM turns. Three tools (`process_start`, `process_read`, `process_stop`) share
a single `JobService` instance so started jobs are visible to all three tools
and the TUI status bar.

### Ownership
- **Source files**: `tools/process_tool.cpp` (3 classes, 240 lines), `lib/job.cpp` (`Job`, `JobService`, reader loop), `lib/process.cpp` (`spawn_shell`, `kill_process_group`)
- **Factory**: `include/agent/tools.h` → `make_process_tools(JobService&)` returns `vector<unique_ptr<Tool>>`
- **Test files**: `tests/run_tests.cpp` — 3 process-tool tests (lines 1320–1362)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `process_start`: `{"command":<string>, "timeout":<optional hard timeout, default 600>, "idle_timeout":<optional idle timeout, default 30>, "cwd":<optional string>}`. `process_read`: `{"id":<string>, "all":<optional bool>}`. `process_stop`: `{"id":<string>}`. |
| **Output** | `process_start`: job ID string. `process_read`: status line + buffered output. `process_stop`: final output before kill. All via `ToolResult`. |
| **Error states** | Missing command/id, job not found, spawn failure. |
| **Invariants** | See below. |
| **Thread safety** | `JobService` is internally synchronised (mutex for job map). `Job::reader_loop` runs on a background thread. Tool execution blocks the agent thread. |

### Invariants

1. All three tools share the same `JobService` instance — started processes are visible to all.
2. `process_start` requires approval (`requires_approval() = true`).
3. `process_read` is read-only (`is_read_only() = true`) and never needs approval.
4. `process_stop` requires approval (`requires_approval() = true`).
5. Job output is capped at 1 MiB per job. Excess is discarded (not returned).
6. `process_read` with `all=true` returns full output; `all=false` returns only new output since last read (delta).
7. Stopped/killed jobs are removed from `JobService` after `process_stop` or when `stop()` is called.

---

### Scenarios

#### [PT-01] Start a background process

- **Given**: A valid command
- **Input**: `process_start` with `{"command": "sleep 10"}`
- **Expected**: `ok=true`. Output is job ID string (e.g., `"job_1"`). Meta: `{"job_id": "job_1"}`. Job registered in shared JobService.
- **Approval**: Requires user approval (tagged as a side-effect operation).

#### [PT-02] Start with custom timeouts

- **Given**: A command that may run long
- **Input**: `{"command": "slow-build.sh", "timeout": 1200, "idle_timeout": 60}`
- **Expected**: Hard timeout = 1200s, idle timeout = 60s. Job runs within these limits.
- **Defaults**: hard=600s, idle=30s if not specified.

#### [PT-03] Read a running job's output

- **Given**: Job started, has produced some output
- **Input**: `process_read` with `{"id": "job_1"}`
- **Expected**: `ok=true`. Status line: `"[job job_1 running idle 30s hard 600s]"`. Body: buffered output since last read (delta) or full output if `all=true`.
- **No new output**: Body: `"(no new output)\n"`.

#### [PT-04] Read finished job

- **Given**: Job completed with exit code 0
- **Input**: `process_read` with `{"id": "job_1"}`
- **Expected**: Status line: `"[job job_1 done exit 0]"`. Body: full output.
- **On failure**: Status wrong or missing.

#### [PT-05] Read killed job

- **Given**: Job was killed (timeout or process_stop)
- **Input**: `process_read` with `{"id": "job_1"}`
- **Expected**: Status line: `"[job job_1 killed]"`. Body: captured output before kill.

#### [PT-06] Stop a running job

- **Given**: Job running with PID
- **Input**: `process_stop` with `{"id": "job_1"}`
- **Expected**: `ok=true`. Output: `"[job job_1 stopped]\n<full output before kill>"`. Job removed from service. Process group receives `SIGKILL`.
- **Approval**: Requires user approval.

#### [PT-07] Stop a finished job

- **Given**: Job already completed
- **Input**: `process_stop` with `{"id": "job_1"}`
- **Expected**: `stop()` returns true (job removed). Output captured. Subsequent `stop()` returns false (`"no such job: job_1"`).
- **Regression guard**: `job_service_stop_finished_returns_true` test.

#### [PT-08] Job not found

- **Given**: Invalid job ID
- **Input**: `process_read` or `process_stop` with `{"id": "nonexistent"}`
- **Expected**: `ok=false`, `error="no such job: nonexistent"`.

#### [PT-09] Output cap at 1 MiB

- **Given**: Job produces more than 1 MiB of output
- **Input**: Long-running output-heavy command
- **Expected**: Output capped, `truncated_ = true`. Reader loop continues (drains pipe to prevent zombie) but doesn't buffer beyond cap.
- **Regression guard**: `job_service_caps_output_at_one_mib` test.

#### [PT-10] Full lifecycle: start → read → stop

- **Given**: A short-lived command
- **Input**: Start ping → read output → stop
- **Expected**: `start` returns ID. `read` returns output with status. `stop` returns final output and removes job.
- **Regression guard**: `process_tools_share_service` test.

#### [PT-11] Start without command

- **Input**: `{}` or `{"command": ""}`
- **Expected**: `ok=false`, `error="missing 'command'"`.

---

### Cross-references

- **Depends on**: `tools/bash-tool.md` (shared `spawn_shell` / `kill_process_group` implementation)
- **Depended on by**: `agent-loop/tool-dispatch.md`, `tui/event-loop.md` (status bar shows running jobs)
- **Test coverage**: `tests/run_tests.cpp`: `process_tools_share_service` (1320), `process_stop_returns_captured_output` (1341), `job_service_stop_finished_returns_true` (1353), `job_service_caps_output_at_one_mib` (1362)

### Known gaps

1. **No workspace confinement on command** — Process runs with user's full privileges. The `cwd` defaults to workspace root but the command can `cd` anywhere.
2. **No output streaming during execution** — The model must poll via `process_read`. No push mechanism for real-time output.
3. **Job reader thread never interrupts** — Once started, the reader thread runs until the pipe closes, even if the agent/calling code no longer needs it.
4. **`all` vs delta mode** — If the model never reads with `all=true` and only reads delta, the full output is still retained in the Job's internal buffer (until the 1 MiB cap).

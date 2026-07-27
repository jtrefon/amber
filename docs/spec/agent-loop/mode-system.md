## Spec: Mode System

### Purpose
Control tool availability, approval policy, and orchestration depth through
three runtime modes (`read`, `write`, `yolo`). The mode is injected into the
system prompt so the LLM understands its constraints, and enforced in the
dispatch layer.

### Ownership
- **Source files**: `include/agent/config.h` (`AgentMode` enum), `lib/agent.cpp` (`ensure_system_prompt()` — mode suffix), `lib/dispatch.cpp` (mode-gated approval), `tui/tui_input.cpp` (`/mode` / `/policy` command), `src/main.cpp` (CLI mode defaults)
- **Test files**: `tests/run_tests.cpp` — `dispatch_auto_approves_in_write_mode`, `agent_stops_on_repeated_empty_arg_tool_call` (indirect)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `AgentMode` set at construction or changed at runtime via `/mode` |
| **Output** | System prompt suffix describing mode; dispatch enforces tool availability |
| **Error states** | None |
| **Invariants** | See below. |
| **Thread safety** | Mode is a `Config` field read by dispatch and agent; set from TUI main thread before next agent run. |

### Invariants

1. Read mode ONLY allows `is_read_only() == true` tools. All other tools return `"not available in read mode"`.
2. Write mode allows all tools with auto-approval (no interactive approval gate).
3. Yolo mode allows all tools with auto-approval.
4. The system prompt suffix reflects the current mode on every `ensure_system_prompt()` call.
5. Changing mode at runtime regenerates the system prompt on next `run()`.

---

### Scenarios

#### [MD-01] Read mode — read-only tools only

- **Given**: `cfg.mode = Read`
- **Input**: Model calls `read("foo.txt")` then `bash("ls")`
- **Expected**: `read` executes (is_read_only=true). `bash` is denied at Gate 1 (mode=Read && !is_read_only()). System prompt says "You are in READ mode. You can only use read-only tools."
- **On failure**: Write tool executes in read mode.

#### [MD-02] Write mode — auto-approve all

- **Given**: `cfg.mode = Write`
- **Input**: Model calls `bash("rm file")` then `read("foo.txt")`
- **Expected**: Both execute. No approval dialog. System prompt: "You are in WRITE mode. All tools run without interactive approval."
- **On failure**: Approval dialog shown in write mode.

#### [MD-03] Yolo mode — full trust

- **Given**: `cfg.mode = Yolo`
- **Input**: Model calls `bash("curl ... | sh")`
- **Expected**: Executes immediately. No approval. System prompt: "You are in YOLO mode. All tools run without approval and execute immediately with full system access."
- **On failure**: Approval dialog shown.

#### [MD-04] Runtime mode switch via `/mode`

- **Given**: TUI user runs `/mode write` then `/mode read`
- **Input**: `/set policy read` or `/mode read`
- **Expected**: `cfg_.mode` updated. On next `send_async()`, `ensure_system_prompt()` re-injects the correct mode suffix. Dispatch enforces the new policy.
- **On failure**: Mode change not reflected until agent restart.

#### [MD-05] Mode switch during active agent

- **Given**: Agent busy, user types `/mode read`
- **Input**: Mode changed while agent is running
- **Expected**: Pending agent continues with its captured mode (mode is read per-turn, not mid-turn). New prompt uses new mode.
- **On failure**: Mid-turn mode change corrupts approval state.

#### [MD-06] Read mode — system prompt includes constraint

- **Given**: `cfg.mode = Read`
- **Input**: `ensure_system_prompt()` called
- **Expected**: System message ends with: "You are in READ mode. You can only use read-only tools..."
- **On failure**: No mode suffix, model attempts write tools.

#### [MD-07] CLI mode via `--yes` / `--yolo`

- **Given**: CLI `amber --yolo --prompt "..."` or `amber --yes --prompt "..."`
- **Input**: `--yolo` or `--yes` flag
- **Expected**: `--yolo` sets `cfg_.mode = Yolo`. `--yes` keeps Write mode but sets `auto_approve = true` for the approval hook.
- **On failure**: Mode not applied.

---

### Cross-references

- **Depends on**: `agent-loop/tool-dispatch.md` (enforcement), `agent-loop/core-loop.md` (system prompt)
- **Depended on by**: `workspace/security-model.md`
- **Test coverage**: `dispatch_auto_approves_in_write_mode` test.

### Known gaps

1. **Approval gate is dead code** — The `mode == Read` gate check at dispatch.cpp:172 is never reached because Gate 1 blocks non-read-only tools first, and no read-only tool requires approval.
2. **Read mode still allows bash read-only commands** — `bash("ls")` is `!is_read_only()`, so it is blocked at Gate 1. But the model could call `read` or `search` for the same effect.

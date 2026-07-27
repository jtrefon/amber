## Spec: Tool Dispatch

### Purpose
Receive tool calls from the LLM reply, validate arguments, run them through
the approval gate (mode-aware), execute approved calls in parallel via
`std::async`, collect results, format them into the feedback envelope, and
push tool-role messages into conversation history. Also detects duplicate
calls within the same turn.

### Ownership
- **Source files**: `lib/dispatch.cpp`, `include/agent/dispatch.h`, `lib/agent_helpers.cpp` (`format_tool_envelope`)
- **Test files**: `tests/run_tests.cpp` — 4 dispatch tests (lines 1381–1540)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `json` array of tool calls (id, name, arguments) + `Config` + `ToolRegistry` + `AgentHooks` + history |
| **Output** | `bool all_ok` — false if any tool returned `ok=false`. Side-effect: tool results pushed to `history_` as `role: "tool"` messages. |
| **Error states** | Unknown tool → denied with `"tool not found"`. Malformed args → denied. Duplicate → denied. All denied calls produce `ToolResult{ok, meta[denied]=true}`. |
| **Invariants** | See below. |
| **Thread safety** | Approved calls dispatched via `std::async(std::launch::async)`. Results polled from main agent thread. Each tool must be thread-safe (stateless or internally synchronised). |

### Invariants

1. Every tool call produces exactly one tool-role message in `history_` — either a real result or a denied/error stub.
2. Tool calls are dispatched in parallel but results are appended to `history_` in the original call order (not completion order).
3. The approval gate runs before any tool executes — no tool starts before its approval is resolved.
4. Unknown tools are denied with `"tool not found: <name>"` — not silently skipped.
5. Malformed JSON arguments produce `"tool call <N>: invalid JSON in arguments"`.
6. The feedback envelope `format_tool_envelope()` wraps every result: `[tool=name args={...} status=ok|error|denied|timeout meta={...}]`.
7. Duplicate detection (when enabled) only blocks calls with identical `(name, args)` within the LAST assistant turn, not all history.

---

### Scenarios

#### [TD-01] Single tool call — approved and executed

- **Given**: LLM response with one valid `read` call
- **Input**: `[{"id":"1","function":{"name":"read","arguments":"{\"path\":\"foo.txt\"}"}}]`
- **Expected**: Tool looked up in registry → args parsed as valid JSON → approval gate passes → `std::async` launches `ReadTool::execute()` → result collected → formatted via envelope → `history_.push_back(tool_msg)`.
- **On failure**: Tool not executed, or result not pushed to history.

#### [TD-02] Multiple tool calls — parallel execution

- **Given**: LLM requests `read(a.txt)` and `read(b.txt)` in same turn
- **Input**: Two tool calls in the same array
- **Expected**: Both dispatched simultaneously via `std::async`. Results polled with 10ms spin. History gets two tool messages in the original call order. `all_ok` is `true` (both succeeded).
- **On failure**: Sequential execution (one `std::async` at a time), or results appended in completion order.

#### [TD-03] Unknown tool name

- **Given**: LLM calls a non-existent tool
- **Input**: `{"name":"nonexistent","arguments":"{}"}`
- **Expected**: `parse_tool_call()` → `registry_.find("nonexistent")` returns null → denied with `"tool not found: nonexistent"`. Envelope: `status=denied meta={"denied":true}`.
- **On failure**: Crash from null dereference.

#### [TD-04] Malformed JSON arguments

- **Given**: Arguments string is not valid JSON
- **Input**: `"arguments": "{bad json}"`
- **Expected**: `parse_tool_call()` → `json::parse` returns discarded → set `args_ok = false`. Envelope: `"tool call <N>: invalid JSON in arguments"`.
- **On failure**: Tool receives empty args or garbage.

#### [TD-05] Read mode blocks non-read-only tools

- **Given**: `cfg.mode = AgentMode::Read`, LLM calls `write`
- **Input**: `{"name":"write","arguments":"..."}`
- **Expected**: Gate 1 check: `!c.tool->is_read_only()` → denied. Envelope: `status=denied meta={"denied":true} reason="tool write not available in read mode"`.
- **On failure**: Write executes in read mode.

#### [TD-06] Yolo mode auto-approves all

- **Given**: `cfg.mode = AgentMode::Yolo`, LLM calls `bash` with `rm -rf`
- **Input**: `{"name":"bash","arguments":{"command":"rm -rf /"}}`
- **Expected**: Gate 3 skipped (mode != Read). `c.approved = true`. Tool executes. No approval dialog.
- **On failure**: Dialog blocks or tool denied.

#### [TD-07] Duplicate detection (same turn)

- **Given**: `cfg.detection_duplicate = true`, LLM calls `read(foo)` twice
- **Input**: Two identical tool calls
- **Expected**: Second call: `find_duplicate_call()` finds match in earlier non-denied call in same assistant turn → denied with `"duplicate call: read(foo)"`.
- **On failure**: Duplicate executes, wasting turn.

#### [TD-08] Duplicate with previous denial retry

- **Given**: `cfg.detection_duplicate = true`, previous call to `bash(rm -rf /)` was denied
- **Input**: Same call again
- **Expected**: `find_duplicate_call()` checks if previous match was denied → if so, allows retry (model may have fixed args or user may have extended permissions).
- **On failure**: User cannot retry a denied command after fixing it.

#### [TD-09] Tool execution throws exception

- **Given**: `Tool::execute()` throws `std::runtime_error`
- **Input**: Any tool call
- **Expected**: `std::async` lambda catches: `catch (...) { return ToolResult{false, "", "tool threw: ..."}; }`. Envelope: `status=error`.
- **On failure**: Exception propagates to agent thread.

#### [TD-10] No approval handler — fail-safe deny

- **Given**: `hooks.on_approval` is null, tool requires approval
- **Input**: `bash` with write command
- **Expected**: `approve_tool()` → `if (!hooks.on_approval) return false` → denied.
- **On failure**: Tool auto-approved without handler (dangerous default).

#### [TD-11] Session-wide approval

- **Given**: User selects "Allow for this session"
- **Input**: Any bash call requiring approval
- **Expected**: `approve_tool()` returns `AllowSession`. `session_approved` set contains tool name. Subsequent calls to same tool skip approval gate.
- **On failure**: User prompted every time.

#### [TD-12] Multiple tool calls — some fail, others succeed

- **Given**: Three calls: `read(a)`, `read(b)`, `read(nonexistent)`
- **Input**: Mixed success/failure
- **Expected**: All three execute in parallel. Two succeed, one fails. `all_ok = false`. All three results pushed to history (even failed ones).
- **On failure**: Failed call cancels siblings.

---

### Cross-references

- **Depends on**: `agent-loop/mode-system.md` (approval policy), `agent-loop/core-loop.md` (loop integration), `tools/read-tool.md` (example tool contract)
- **Depended on by**: `agent-loop/error-recovery.md` (FailStreak reads dispatch results)
- **Test coverage**: `tests/run_tests.cpp`: `dispatch_approves_and_runs_valid_tool_call`, `dispatch_rejects_duplicate_tool_call`, `dispatch_auto_approves_in_write_mode`, `dispatch_missing_tool_reports_unknown`

### Known gaps

1. **Approval gate (Gate 3) is dead code** — Conditioned on `mode == Read`, but Read mode blocks non-read-only tools at Gate 1, and no read-only tool requires approval. The `on_approval` hook is never reached through the dispatch path.
2. **`session_approved_` never cleared** — Lasts for Agent lifetime. Only `reset()` clears it.
3. **No resource limits on parallel dispatch** — If LLM requests 50 tool calls, all 50 are dispatched simultaneously.
4. **Results polled with busy-spin** — Uses `wait_for(0)` with 10ms sleep. Not efficient for long-running tools.

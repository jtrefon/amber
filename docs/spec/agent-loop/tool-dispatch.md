## Spec: Tool Dispatch

### Purpose
Receive tool calls from the LLM reply, validate arguments, consult the
PolicyStore for the effective permission level (Write mode), run the
approval gate, execute approved calls in parallel via `std::async`,
collect results, format them into the feedback envelope, and push tool-role
messages into conversation history.

### Ownership
- **Source files**: `lib/dispatch.cpp`, `include/agent/dispatch.h`, `lib/policy.cpp`, `lib/agent_helpers.cpp`
- **Test files**: `tests/run_tests.cpp` — 4 dispatch tests

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `json` tool calls + `Config` + `ToolRegistry` + `AgentHooks` + `PolicyStore*` + history |
| **Output** | `bool all_ok` — false if any call failed. Side-effect: tool results pushed to `history_`. |
| **Error states** | Unknown tool → denied. Malformed args → denied. Duplicate → denied. Policy deny → denied. |
| **Invariants** | See below. |

### Invariants

1. Every tool call produces exactly one tool-role message in `history_`.
2. Tool calls are dispatched in parallel but results appended in original order.
3. The approval gate runs before any tool executes.
4. Unknown tools are denied with `"tool not found: <name>"`.
5. Malformed JSON arguments produce `"tool call <N>: invalid JSON in arguments"`.
6. The feedback envelope wraps every result with `status=ok|error|denied`.
7. Duplicate detection only blocks identical calls within the LAST assistant turn.

### Approval gate flow

```
for each tool call c:
  1. registry.find(c.fn) — unknown → deny
  2. if Read mode && !is_read_only() → deny
  3. if Yolo mode → approve unconditionally
  4. if policy_approval off → approve unconditionally
  5. if !requires_approval() → approve (read-only commands)
  6. if session_approved contains c.fn → approve
  7. if policy store has matching rule:
       • always_allow → show dialog with Allow pre-selected, short timer
       • always_deny  → show dialog with Deny pre-selected, short timer
       • ask          → show dialog with last-choice default, 60s timer
  8. Dialog returns one of: Deny, AllowOnce, AllowSession, AlwaysAllow, AlwaysDeny
  9. Store result: record_choice(), save session grant if AllowSession,
     set_rule() if AlwaysAllow/AlwaysDeny
```

---

### Scenarios

#### [TD-01] Single tool call — approved and executed
Standard flow: parse → lookup → gate passes → execute → format → push.

#### [TD-02] Multiple tool calls — parallel execution
Dispatched via `std::async`. Results polled with 10ms spin. Original order preserved.

#### [TD-03] Unknown tool name
Denied with `"tool not found: nonexistent"`. Envelope `status=denied`.

#### [TD-04] Malformed JSON arguments
`parse_tool_call()` → `json::parse` returns discarded → `args_ok=false` → denied.

#### [TD-05] Read mode blocks non-read-only tools
Gate 1 blocks write tools. PolicyStore not consulted (read-only tools never require approval).

#### [TD-06] Write mode — PolicyStore consulted
- **Given**: `cfg.mode = Write`, `policy_approval = true`, `bash` requires approval
- **Input**: `bash("rm file")`
- **Expected**: Gate 2 passes (not Read mode). Gate 3 checks PolicyStore → `ask`. Dialog shown. User picks AllowOnce → tool executes.
- **On failure**: Tool runs without dialog.

#### [TD-07] Write mode — AlwaysAllow rule
- **Given**: PolicyStore has `bash → allow`
- **Input**: `bash("rm file")`
- **Expected**: Gate 3 → PolicyStore → `always_allow`. Dialog shows with Allow pre-selected. Short countdown auto-confirms.
- **On failure**: Silent auto-approve (no last-chance dialog).

#### [TD-08] Write mode — AlwaysDeny rule
- **Given**: PolicyStore has `bash → deny`
- **Input**: `bash("rm file")`
- **Expected**: Gate 3 → PolicyStore → `always_deny`. Dialog shows with Deny pre-selected. Countdown auto-denies.
- **On failure**: Tool executes despite deny rule.

#### [TD-09] Yolo mode bypasses all gates
Gate 3: `cfg.mode != Yolo` → skips approval entirely.

#### [TD-10] policy_approval toggle off
Gate 3: `cfg.policy_approval` is false → approve unconditionally (legacy).

#### [TD-11] Duplicate detection
`find_duplicate_call()` catches exact repeats within the last assistant turn. Previous denials allow retry.

#### [TD-12] AllowSession grant
`approve_tool()` returns `AllowSession` → `session_approved` set + `policy_.grant_session()`. Future calls skip gate.

#### [TD-13] AlwaysAllow / AlwaysDeny from dialog
User picks "Always Allow" in dialog → `policy_.set_rule(name, AlwaysAllow)` and saves to `.amber/policy.json`.

#### [TD-14] No approval handler — fail-safe deny
`hooks.on_approval` is null → `approve_tool()` returns false.

---

### Cross-references

- **Depends on**: `agent-loop/mode-system.md`, `lib/policy.cpp`, `tui/confirm_panel.cpp`
- **Depended on by**: `agent-loop/error-recovery.md`
- **Test coverage**: 4 dispatch tests + PolicyStore tests

### Resolved gaps

1. ~~Approval gate (Gate 3) is dead code~~ — Reinstated with PolicyStore. Gate now checks `requires_approval()` in Yolo/Write mode; Yolo bypasses, Write consults PolicyStore.
2. ~~`session_approved_` never cleared~~ — Now cleared via `policy_.clear_session()` on `/new`.

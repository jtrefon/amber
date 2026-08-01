## Spec: Mode System

### Purpose
Control tool availability and approval policy through three runtime modes
(`read`, `write`, `yolo`). The mode is injected into the system prompt so the
LLM understands its constraints, and enforced in the dispatch layer.

### Ownership
- **Source files**: `include/agent/config.h` (`AgentMode` enum), `lib/agent.cpp` (`ensure_system_prompt()`), `lib/dispatch.cpp` (mode-aware approval gate), `lib/policy.cpp` (`PolicyStore`), `tui/tui_input.cpp` (`/set policy`), `src/main.cpp` (CLI defaults)
- **Test files**: `tests/run_tests.cpp` — `dispatch_auto_approves_in_write_mode`

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `AgentMode` set at construction or changed at runtime via `/set policy mode` |
| **Output** | System prompt suffix; dispatch enforces tool availability + PolicyStore approval |
| **Error states** | None |
| **Thread safety** | Mode is a `Config` field read by dispatch; set from TUI main thread. PolicyStore is Agent-scoped. |

### Invariants

1. Read mode ONLY allows `is_read_only() == true` tools. Non-read-only tools return `"not available in read mode"`. PolicyStore is NOT consulted — read-only tools never require approval.
2. Write mode allows all tools. Approval-gated tools (those where `requires_approval() == true`) consult the **PolicyStore** for the effective permission level. See scenarios below.
3. Yolo mode allows all tools with auto-approval. PolicyStore and approval dialogs are bypassed entirely.
4. The `policy_approval` master toggle (Config) controls whether the PolicyStore is active in Write mode. When off, Write mode reverts to legacy auto-approve behavior.
5. The system prompt suffix reflects the current mode on every `ensure_system_prompt()` call.
6. Changing mode at runtime regenerates the system prompt on next `run()`.

---

### Security Model

```
Mode      PolicyStore active?   Approval dialog?   Behavior
──────    ───────────────────   ────────────────   ────────
Read      No (bypass)           No                 Read-only tools only
Write     Yes (if toggle on)    Yes (if Ask)       Consult PolicyStore:
                                                    • AlwaysAllow → auto-approve
                                                    • AlwaysDeny  → deny silently
                                                    • Ask         → show ApprovalPanel
                                                    • AllowSession → skip rest of session
         No (toggle off)        No                 Legacy: auto-approve all
Yolo      No (bypass)           No                 Full speed, no gates
```

### PolicyStore rules

Rules are stored in `.amber/policy.json`. Each rule has a name (tool name), a
level (`allow`, `deny`, or `ask`), a last-choice tracker, and a usage counter.

The shipped default contains 30+ entries for potentially destructive patterns
(`rm`, `dd`, `git reset`, `docker`, `npm install`, etc.) all set to `ask`.

Users manage rules via:
- `/set policy rule <name> <allow|deny|ask>` — set or revoke a rule
- `/get policy rule <name>` — see a specific rule
- `/get policy` — list all rules

---

### Scenarios

#### [MD-01] Read mode — read-only tools only

- **Given**: `cfg.mode = Read`
- **Input**: Model calls `read("foo.txt")` then `bash("ls")`
- **Expected**: `read` executes. `bash` is denied at Gate 1 (mode=Read && !is_read_only()). System prompt says "You are in READ mode. You can only use read-only tools."
- **On failure**: Write tool executes in read mode.

#### [MD-02] Write mode — PolicyStore gates approval

- **Given**: `cfg.mode = Write`, `policy_approval = true`, PolicyStore has `bash → ask`
- **Input**: Model calls `bash("rm file")`
- **Expected**: `requires_approval()` returns true. PolicyStore lookup returns `ask`. ApprovalPanel dialog appears with last-choice default and 60s countdown. User picks `AllowOnce` → tool executes, last-choice updated.
- **On failure**: Tool runs without dialog when policy_approval is on.

#### [MD-03] Write mode — AlwaysAllow rule skips dialog

- **Given**: `cfg.mode = Write`, PolicyStore has `bash → allow`
- **Input**: Model calls `bash("rm file")`
- **Expected**: `requires_approval()` returns true. PolicyStore lookup returns `allow`. ApprovalPanel appears with "Allow" pre-selected. Short countdown auto-confirms. User can override.
- **On failure**: Dialog never appears (would violate "always show last-chance safety net").

#### [MD-04] Write mode — AlwaysDeny rule blocks

- **Given**: `cfg.mode = Write`, PolicyStore has `bash → deny`
- **Input**: Model calls `bash("rm file")`
- **Expected**: `requires_approval()` returns true. PolicyStore lookup returns `deny`. ApprovalPanel appears with "Deny" pre-selected. Short countdown auto-denies.
- **On failure**: Tool executes despite deny rule.

#### [MD-05] Write mode — AllowSession grant

- **Given**: `cfg.mode = Write`, PolicyStore has `bash → ask`
- **Input**: Model calls `bash("rm file")`. User picks "Allow Session" in dialog.
- **Expected**: First call shows dialog. Further bash calls within same session skip dialog (session grant cached in PolicyStore). New session resets grants.
- **On failure**: Dialog shown every time during same session.

#### [MD-06] Write mode — policy_approval off (legacy)

- **Given**: `cfg.mode = Write`, `policy_approval = false`
- **Input**: Model calls `bash("rm file")`
- **Expected**: Approval gate is skipped. Tool runs without any prompt (pre-policy behavior).
- **On failure**: Dialog appears despite toggle being off.

#### [MD-07] Yolo mode — full trust

- **Given**: `cfg.mode = Yolo`
- **Input**: Model calls `bash("curl ... | sh")`
- **Expected**: Executes immediately. No PolicyStore check. No approval dialog. System prompt: "You are in YOLO mode. All tools run without approval..."
- **On failure**: Approval dialog shown.

#### [MD-08] Runtime mode switch

- **Given**: TUI user runs `/set policy mode write` then `/set policy mode read`
- **Expected**: `cfg_.mode` updated. Next `send_async()` re-injects the correct mode suffix. Dispatch enforces the new policy.
- **On failure**: Mode change not reflected until agent restart.

#### [MD-09] CLI mode flags

- **Given**: CLI `amber --yolo --prompt "..."` or `amber --yes --prompt "..."`
- **Input**: `--yolo` / `--yes` flag
- **Expected**: `--yolo` sets `cfg_.mode = Yolo`. `--yes` keeps Write mode but sets `auto_approve = true`.
- **On failure**: Mode not applied.

---

### Cross-references

- **Depends on**: `agent-loop/tool-dispatch.md` (PolicyStore gate), `lib/policy.cpp`
- **Depended on by**: `workspace/security-model.md`
- **Test coverage**: `dispatch_auto_approves_in_write_mode` (updated for new gate), `dispatch_approves_and_runs_valid_tool_call`

### Resolved gaps

1. ~~Approval gate is dead code~~ — Reinstated with PolicyStore consultation in Write mode. Read-only tools don't reach it (their `requires_approval()` returns false). Yolo bypasses it.
2. ~~Read mode blocks bash even for benign commands~~ — Read mode correctly blocks non-read-only tools at Gate 1. Users switch to Write mode for shell access.

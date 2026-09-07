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
2. Write mode allows all tools, but the approval decision is scoped per command kind, not per tool: bash invocations are classified (`shell_classify`) into ReadOnly / benign in-workspace Write / Destructive / Outside. ReadOnly and benign Write run free; Destructive and Outside prompts (see scenarios).
3. Yolo mode allows all tools with auto-approval. PolicyStore and approval dialogs are bypassed entirely.
4. The `policy_approval` master toggle (Config) controls whether the PolicyStore is active in Write mode. When off, Write mode reverts to legacy auto-approve behavior.
5. Stored rules are scope-keyed (`bash:rm`, `bash:git reset`, `outside:/abs/dir`, bare tool names for non-bash tools): `AlwaysAllow`/`AlwaysDeny` skip the dialog entirely (no re-prompt — this is the fix for the "granted always but asked again" bug); `AllowSession` grants only that scope.
6. The system prompt suffix reflects the current mode on every `ensure_system_prompt()` call.
7. Changing mode at runtime regenerates the system prompt on next `run()`.

---

### Security Model

```
Mode      PolicyStore active?   Approval dialog?   Behavior
──────    ───────────────────   ────────────────   ────────
Read      No (bypass)           No                 Read-only tools only
Write     Yes (if toggle on)    Yes (if Ask)       Consult PolicyStore by scope:
                                                    • AlwaysAllow → run silently
                                                    • AlwaysDeny  → deny silently
                                                    • Ask         → show ApprovalPanel
                                                    • AllowSession → skip rest of session
         No (toggle off)        No                 Legacy: auto-approve all
Yolo      No (bypass)           No                 Full speed, no gates
```

### PolicyStore rules

Rules are stored in `.amber/policy.json`. Each rule is keyed by a **scope id**:
a command kind for bash (`bash:rm`, `bash:git reset`, `bash:*` for
unclassifiable), an outside-workspace folder (`outside:/abs/dir`), or a bare
tool name for non-bash gated tools (`write`, `process_start`). Each rule has a
level (`allow`, `deny`, or `ask`), a last-choice tracker, and a usage counter.

The shipped default seeds the curated destructive-command list (`rm`, `dd`,
`git reset`, `docker`, `npm install`, ...) all at `ask` — those commands are
the ones that pause for approval in Write mode; benign in-workspace commands
(`ls`, `wc`, `echo x > f`, `mv` inside the workspace) never prompt.

Users manage rules via:
- `/set policy rule <name> <allow|deny|ask>` — set or revoke a rule; names
  complete from the registry and the curated pattern list (`bash.rm`, ...)
- `/get policy rule <name>` — see a specific rule
- `/get policy` — list all rules

---

### Scenarios

#### [MD-01] Read mode — read-only tools only

- **Given**: `cfg.mode = Read`
- **Input**: Model calls `read("foo.txt")` then `bash("ls")`
- **Expected**: `read` executes. `bash` is denied at Gate 1 (mode=Read && !is_read_only()). System prompt says "You are in READ mode..."
- **On failure**: Write tool executes in read mode.

#### [MD-02] Write mode — destructive command prompts

- **Given**: `cfg.mode = Write`, `policy_approval = true`, PolicyStore has `bash:rm → ask`
- **Input**: Model calls `bash("rm file")`
- **Expected**: `requires_approval()` returns true. Classification is Destructive with scope `bash:rm`. Decision engine returns Prompt (no stored rule, no session grant). ApprovalPanel dialog appears. User picks `AllowOnce` → tool executes.
- **On failure**: Tool runs without dialog when policy_approval is on.

#### [MD-03] Write mode — AlwaysAllow rule skips the dialog

- **Given**: `cfg.mode = Write`, PolicyStore has `bash:rm → allow`
- **Input**: Model calls `bash("rm file")`
- **Expected**: `requires_approval()` returns true. Decision engine returns Allow — **no dialog appears** (stored always-allow is honored, never re-prompts). This is the regression fix for "granted always, asked again".
- **On failure**: Dialog appears despite the stored allow rule.

#### [MD-04] Write mode — AlwaysDeny rule blocks silently

- **Given**: `cfg.mode = Write`, PolicyStore has `bash:rm → deny`
- **Input**: Model calls `bash("rm file")`
- **Expected**: Decision engine returns DenySilent. No dialog; call is denied with a `denied` result so the model can self-recover.
- **On failure**: Tool executes despite deny rule.

#### [MD-05] Write mode — AllowSession grant is per scope

- **Given**: `cfg.mode = Write`, PolicyStore has `bash:rm → ask`
- **Input**: Model calls `bash("rm file")`. User picks "Allow Session" in dialog.
- **Expected**: First call shows dialog. A later `bash("rm other")` in the same session skips the dialog (session grant cached under `bash:rm`), but `bash("dd ...")` still prompts (different scope). New session resets grants.
- **On failure**: Dialog shown every time during same session, or one grant silently approves every command kind.

#### [MD-06] Write mode — benign commands run free

- **Given**: `cfg.mode = Write`, `policy_approval = true`
- **Input**: Model calls `bash("ls -la")`, `bash("echo x > out.txt")`, `bash("git status")`
- **Expected**: No approval dialog: read-only commands and benign in-workspace writes are Allow.
- **On failure**: Benign commands prompt (the reported `ls`/`wc` re-prompt bug).

#### [MD-07] Write mode — outside-workspace writes prompt per folder

- **Given**: `cfg.mode = Write`
- **Input**: Model calls `bash("echo x > /tmp/notes/f")`
- **Expected**: Classification is Outside with scope `outside:/tmp/notes`. First write to that folder prompts; a session grant makes later writes to the same folder silent; a write to a different outside folder prompts again.
- **On failure**: Every outside write prompts, or one grant covers all folders.

#### [MD-08] Yolo mode — full trust

- **Given**: `cfg.mode = Yolo`
- **Input**: Model calls `bash("curl ... | sh")`
- **Expected**: Executes immediately. No PolicyStore check. No approval dialog. System prompt: "You are in YOLO mode..."
- **On failure**: Approval dialog shown.

#### [MD-09] Runtime mode switch

- **Given**: TUI user runs `/set policy mode write` then `/set policy mode read`
- **Expected**: `cfg_.mode` updated. Next `send_async()` re-injects the correct mode suffix. Dispatch enforces the new policy.
- **On failure**: Mode change not reflected until agent restart.

#### [MD-10] CLI mode flags

- **Given**: CLI `amber --yolo --prompt "..."` or `amber --yes --prompt "..."`
- **Input**: `--yolo` / `--yes` flag
- **Expected**: `--yolo` sets `cfg_.mode = Yolo`. `--yes` keeps Write mode but sets `auto_approve = true`. The TTY prompt also offers `[g]rant always` (persisted rule).
- **On failure**: Mode not applied.

---

### Cross-references

- **Depends on**: `agent-loop/tool-dispatch.md` (PolicyStore gate), `lib/policy.cpp`, `lib/shell_classify.cpp`, `lib/policy_engine.cpp`
- **Depended on by**: `workspace/security-model.md`
- **Test coverage**: `policy_engine_modes`, `policy_always_allow_suppresses_dialog`, `policy_always_deny_blocks_silently`, `policy_session_grant_per_scope`, `policy_benign_write_free_in_write_mode`, `policy_outside_scope_prompts_and_grants`, `dispatch_auto_approves_in_write_mode`, `shell_classify_*`

### Resolved gaps

1. ~~Approval gate is dead code~~ — Reinstated with PolicyStore consultation in Write mode. Read-only tools don't reach it (their `requires_approval()` returns false). Yolo bypasses it.
2. ~~Read mode blocks bash even for benign commands~~ — Read mode correctly blocks non-read-only tools at Gate 1. Users switch to Write mode for shell access.
3. ~~AlwaysAllow/AlwaysDeny persisted but never consulted~~ — The decision engine now reads stored rules by scope and skips the dialog for allow/deny (MD-03/MD-04); benign commands no longer prompt (MD-06).

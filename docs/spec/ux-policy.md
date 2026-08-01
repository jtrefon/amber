## UX Policy

### Core principle

Every command has UI feedback; every UI action has a command equivalent.
The single exception is the security/approval dialog — it is UI-only because
it requires real-time human judgment.

### PolicyStore-driven security

The PolicyStore (`lib/policy.cpp`) is a JSON-persisted database of tool-level
permission rules. Each rule maps a tool name to one of three levels:
- `allow` — tool runs with last-chance dialog (auto-confirms on timeout)
- `deny` — tool blocked with last-chance dialog (auto-denies on timeout)
- `ask` — default state, user must actively choose every time

The store ships with 30+ predefined harmful patterns (`rm`, `dd`, `git reset`,
`docker`, `sudo`, `npm install`, etc.) all set to `ask`. Users manage rules
via the `/set policy` and `/get policy` command tree.

### Approval dialog

The `ApprovalPanel` is a 4-option ncurses dialog with keyboard shortcuts:
- `1` Allow once — pass through, no state change
- `2` Allow session — grant for rest of conversation
- `3` Allow (always) — persist rule to PolicyStore
- `4` Deny (always) — persist rule to PolicyStore
- `Esc` — deny this call only

A 60-second countdown timer auto-selects the last-chosen option as the
default. The user can configure the timeout via `/set policy timeout <N>`.

### Mode-based gate behavior

```
Read  → PolicyStore bypassed (read-only tools only, no gate needed)
Write → PolicyStore consulted, approval dialog shown for Ask rules
Yolo  → PolicyStore bypassed (full trust, no road bumps)
```

### Policy commands

```
/set policy mode <read|write|yolo>        — agent access mode
/set policy approval <on|off>            — master toggle for permission gate
/set policy timeout <N>                  — dialog countdown (0 = no timeout)
/set policy rule <name> <allow|deny|ask> — per-tool permission rule

/get policy                              — show mode, timeout, all rules
/get policy rule <name>                  — show specific rule
```

### Namespace-aware drawer

Typing `/get` at the prompt replaces the command list with the namespace's
children, each showing:
```
  name  One-line help text.  [choices|range]
```

Typing `/get policy` descends into the policy namespace. Typing `/get think`
shows the leaf with its available choices `[on|off|auto]`.
Pressing `?` at any depth opens a full manual page popup.

### Habit formation loops

```
Set a per-tool policy:
  /get policy               — list all rules
  /get policy rule bash     — see bash's rule
  /set policy rule bash allow  — allow bash permanently
  /set policy approval off  — disable all permission gating
```

### Design rules

1. Kebab-case for multi-word keys (`compression.threshold`).
2. Fixed-set values use `[choices]` notation shown inline in the drawer.
3. Destructive verbs show confirmation dialog (`confirm_delete`).
4. Mode changes take effect on next agent run, not mid-turn.
5. PolicyStore changes persist immediately to `.amber/policy.json`.

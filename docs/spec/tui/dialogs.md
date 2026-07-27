## Spec: Approval Dialog

### Purpose
Block the agent worker thread with a modal ncurses dialog that presents
approval options to the user, then return the selected `Approval` value.
Supports keyboard shortcuts (`1` `2` `3` `4`), Tab/arrow navigation,
a countdown timer that auto-selects the default, and last-choice tracking.

### Ownership
- **Source files**: `tui/confirm_panel.cpp`, `tui/confirm_panel.h`
- **Used by**: `tui/tui.cpp` `resolve_approval()`, `tui/tui_input.cpp` inline hook

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Tool summary text, timeout seconds, default option index (0-3) |
| **Output** | `agent::Approval` enum value |
| **Error states** | None — Esc maps to `Deny`, timer expiry maps to default |
| **Thread safety** | Called from agent worker thread via promise/future |

### Options

| Key | Label | PolicyLevel | Effect |
|-----|-------|-------------|--------|
| `1` | Allow once | `AllowOnce` | Pass through, no state change |
| `2` | Allow session | `AllowSession` | Grant for rest of conversation |
| `3` | Allow (always) | `AlwaysAllow` | Persist rule to PolicyStore |
| `4` | Deny (always) | `AlwaysDeny` | Persist rule to PolicyStore |
| Esc | Cancel | `Deny` | Deny this call only |

### Timer behavior

- Default timeout: 60 seconds (configurable via `/set policy timeout <N>`)
- 0 = no timeout (wait indefinitely for user input)
- Timer displayed on the default option: `Allow once (45s)`
- On expiry: default option is auto-selected (solidifies last choice)
- Timer ticks are computed from elapsed wall-clock time, checked on each
  keypress (no dedicated timer thread)

### Default selection

The default (pre-highlighted) option is determined from the PolicyStore's
`last_choice` for the tool being approved:
- `AllowOnce` → option 0 (pre-selected for first-time approvals)
- `AllowSession` → option 1
- `AlwaysAllow` → option 2
- `AlwaysDeny` → option 3

### Layout

```
┌──────────────────────────────────────┐
│  Approve action?                     │
│                                      │
│  run: rm -rf /                       │
│                                      │
│  [1] Allow once    (45s)             │
│  [2] Allow session                   │
│  [3] Always allow                    │
│  [4] Always deny                     │
│                                      │
│  1-4:pick  Enter:confirm  Esc:cancel │
└──────────────────────────────────────┘
```

---

### Scenarios

#### [DI-01] Basic approval
Tool requires approval → `approve_dialog()` called with summary and timeout.
Dialog shown. User presses `1` → returns `AllowOnce`. Tool executes.

#### [DI-02] Countdown auto-selects default
User does nothing. After 60s, default option auto-confirms. Dialog closes
as if user pressed Enter on the default.

#### [DI-03] Esc cancels
User presses Esc → returns `Deny`. Call blocked.

#### [DI-04] Keyboard shortcuts
User presses `3` → immediately selects AlwaysAllow without navigating.

#### [DI-05] Tab navigation
User presses Tab → selection cycles through options. Enter confirms.

#### [DI-06] Dialog reuses ConfirmPanel (yes/no)
`confirm_dialog()` used for destructive confirmations (delete session, etc.).
Simple Yes/No toggle with Tab/Enter/Esc.

---

### Cross-references
- **Depends on**: `agent/policy.h` (PolicyLevel enum), `lib/policy.cpp` (last_choice lookup)
- **Depended on by**: `tui/tui.cpp` (resolve_approval), `tui/tui_input.cpp` (inline hook)

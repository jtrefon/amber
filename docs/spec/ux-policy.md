# UX Policy — Interaction Model

**How the user and the agent talk to each other.**

The vision (`VISION.md`) says *why*. The mission (`MISSION.md`) says *what
and when*. This document says *how the interface works* — the rules that
every UI component, every command, every dialog must follow.

---

## Core principle: 1:1 mapping with one exception

**Every action that can be done via the UI must also be achievable via a
command. Every action that can be done via a command must have UI feedback.**

| Direction | Interface | Always? |
|-----------|-----------|---------|
| User → agent | Command line (`/`) | Yes |
| Agent → user | Scrollback (text output) | Yes |
| User configures tool | Command OR UI dialog | Both work |
| User confirms dangerous ops | UI dialog only | **Exception** — cannot be scripted |
| User browses data | Command OR UI | Both work |
| User monitors state | Status bar | Always visible |
| User gets help | `?` at depth | Unified |

The single exception: **security boundaries**. Approval dialogs are UI-only
because they block the agent thread and require explicit user consent. The
CLI equivalent is `--yes` (pre-authorised batch mode).

---

## The text vs UI boundary

The user never asks "should I use a command or a dialog?" — the answer is
always "both work, use what feels natural." This is possible because every
UI flow has a command equivalent and every command has a UI feedback path.

```
User types `/`
     │
     ▼
┌──────────────────────┐
│ Is `?` the last      │
│ token after a space? │──── Yes ──► Show help page for path before `?`
└──────────┬───────────┘
           │ No
           ▼
┌──────────────────────┐
│ Is it a known        │
│ command path?        │──── Yes ──► Resolve tree, dispatch handler
└──────────┬───────────┘           │
           │ No                    ▼
           ▼               ┌──────────────┐
      ┌──────────┐         │ UI feedback  │
      │ Unknown  │         │ • status msg │
      │ command  │──► Error│ • scrollback │
      │ error    │         │ • drawer     │
      └──────────┘         │ • dialog     │
                           └──────────────┘
```

Every command produces at least one form of UI feedback:
- **Status message** for quick confirmation ("detection loop: on")
- **Scrollback entry** for permanent record
- **Dialog** for complex output or choices
- **Drawer update** for contextual feedback during typing

---

## Input system hierarchy

```
Layer 1: Readline key bindings  (Ctrl-A/E/B/F/W/U/K/Y/L/T/_, Alt-B/F/D)
         Basic text editing. Always available. Standard shell UX.

Layer 2: Autosuggest shadow      (fish/zsh style)
         Completion hint faded in the input line. Updates on every keystroke.
         Tab accepts the shadow. No need to Tab to "see" it.

Layer 3: Tab cycling             (zsh menu selection)
         Tab accepts shadow then cycles through alternatives inline.
         Shift-Tab cycles reverse. The drawer follows the cycle.

Layer 4: Choice commands         (Ctrl-D list-all, Ctrl-R history search)
         Ctrl-D: show all completions without modifying input.
         Ctrl-R: incremental reverse history search.

Layer 5: `?` help at depth       (typed `?` as last token)
         Opens full help page for the path before `?`.
         `/set detection loop ?` — not `/help set detection loop`.
```

---

## The `?` help system

`?` has two modes, distinguished by the character before it:

| Typed | Mode | Behaviour |
|-------|------|-----------|
| `/set detection loop?` | **No space** — inline popup | Quick popup of remaining options for the current position (e.g., `on`, `off`, `toggle`). `?` stripped, input unchanged. |
| `/set detection loop ?` | **Space** — full page | Full help page for the resolved path before `?`. Usage, args, types, choices, current values, flags, related commands. |

The space is the differentiator. No space = "help for what I'm typing right
now". Space = "help for everything I already typed".

`/help [path]` is kept as sugar for `?` at root.

Examples:

| What you type | What you get |
|---------------|-------------|
| `/ ?` | Full help for root |
| `/set?` | Inline popup of subcommands |
| `/set ?` | Full help page for `set` |
| `/set detection loop?` | Inline popup: `on`, `off`, `toggle` |
| `/set detection loop ?` | Full help page for `loop` (choices + current value) |
| `/config network proxy?` | Inline popup: value spec for key-chain |
| `/config network proxy ?` | Full help page showing key path + value spec |

To pass `?` as a literal value: type without space before it.
`/set val?` passes `val?` as the value (no space = no help trigger).

---

## Widget policy

| Widget | Used for | Rationale |
|--------|----------|-----------|
| `menu_select()` | Approval, multi-choice, completion popup | Simple, familiar, keyboard-first |
| `form_edit()` | Provider config, multi-field settings | Grouped input, Tab between fields |
| `info_dialog()` | Help pages, errors, status | Read-only display, dismiss on any key |
| `list_panel()` | Model list, session browser, file chooser | Scrollable selection, Up/Down/Enter/Esc |
| Status bar | Activity, token usage, jobs, clock, scroll% | Always visible, real-time, glanceable |
| Drawer | Autosuggest, contextual help, completion list | Context-aware overlay, appears on `/` |
| Scrollback | Agent output, command results, conversation history | Permanent record, scrollable, markdown-rendered |

---

## Design rules for commands

1. Every command that accepts arguments also works with `?` for help at
   that node.
2. Every CRUD operation has both a command path and a UI path.
3. Commands use kebab-case names (`detection-loop`, `save-session`).
4. Arguments with a fixed set of values use `<choice|choice|choice>` syntax
   and Choice `ArgSpec`.
5. Every command handler produces a status message.
6. Destructive verbs (`delete`, `kill`, `stop`) show confirmation. The
   user confirms via dialog or Enter.
7. Shortcut aliases follow the BitchX/IrcII scheme: single-letter for
   top-level (`/q`=quit), glued two-letter for nested (`/sl`=session list).

---

## Habit formation loops

Users form habits when actions are predictable:

```
Change a setting:
  /set ↵          — drawer shows categories
  /set det ↵      — shadow shows detection
  /set detection lo Tab  — shadow accepts, loop
  /set detection loop Tab → on  — cycle to value
  Enter            — dispatches
  "detection loop: on"  — status confirms
```

```
Browse a file:
  /files ls src/ ↵   — scrollback shows listing
  /files open src/main.cpp ↵ — file content in pager
  /ff *.rs ↵         — shortcut for files find
```

```
Kill a stuck job:
  /job ls ↵          — shows running jobs
  /job kill 3 ↵      — kills job 3
  "job 3: killed"    — status confirms
```

```
Get help:
  /set ? ↵           — help for set node
  /set detection ? ↵ — help for detection node
  Esc                — dismiss help, back to /set detection
```

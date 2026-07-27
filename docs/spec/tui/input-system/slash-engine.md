## Spec: Slash Engine (Command Dispatch)

### Purpose
Detect when the user types a `/` command in the TUI input line, match it
against the registered command tree, and dispatch to the handler. Supports
live filtering (BitchX-style drawer that narrows per depth level),
tab-completion with shadow, keyboard navigation, and a Cisco IOS-style `?`
key to show remaining options at any depth.

### Inspiration
- **BitchX**: drawer pops on `/`, filters as you type, shows usage + description
- **zsh completion**: menu-selection with Tab cycling, shadow completion extends
  to unambiguous prefix before showing choices
- **Cisco IOS `?`**: type `?` at any point to see what tokens are valid next,
  including their descriptions and current values

### Ownership
- **Source files**: `tui/tui_input.cpp` (command definitions, handler functions), `tui/palette.h`, `tui/palette.cpp` (`Command`, `Subcommand`, `Completer`, free helpers), `tui/tui_render.cpp` (draw_drawer)
- **Test files**: `tests/tui_tests.cpp` (palette helpers, Completer), `completion/tests/completion_test.cpp` (standalone tree-based completion library — unused by TUI)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Any printable character, Tab, Enter, Up/Down, Esc, `?` in TUI event loop |
| **Output** | Commands dispatched to handler functions; drawer overlay updated; input buffer modified |
| **Error states** | Unknown command → status message; ambiguous prefix → drawer or popup |
| **Thread safety** | All input handling on the main (render) thread. |

### Invariants

1. Any input starting with `/` triggers the drawer overlay.
2. The drawer shows a filtered view that narrows as more characters are typed.
3. At each depth level, the drawer shows: available subcommands/args, their usage, description, and current value (for settings).
4. Tab accepts the current autosuggest shadow. Subsequent Tabs cycle inline through alternative matches (zsh-style). Shift-Tab cycles reverse.
5. `?` as the last token (typed after a space, e.g., `/set ?`) opens the full help page for the resolved path before `?`. Works at any depth.
6. Enter dispatches the drawer selection (if open) or the full command line.
7. Esc closes the drawer without executing.
8. **Up/Down arbitration** (zsh-based rule):
   - Input empty → Up/Down = prompt history (as before)
   - Input has content AND drawer has active matches → Up/Down = cycle through
     matches (same as Tab/Shift-Tab). The drawer selection and cycle index are
     synced — moving one updates the other.
   - Input has content AND drawer has NO matches → Up/Down = prompt history
     (filtered by input prefix, zsh-style)
9. **`/no` prefix** (Cisco IOS-style): `/no set detection loop` is equivalent to `/set detection loop off`. The `no` prefix inverts or negates the command.
10. **Partial command acceptance** (Cisco IOS-style): if the typed prefix at any depth is **unambiguous**, Enter dispatches without requiring Tab-complete first. E.g., `/se` that uniquely resolves to `/set` is accepted as `/set` on Enter.

---

### Scenarios

#### [SE-01] Trigger drawer on `/` — show all commands

- **Given**: Input buffer is empty
- **Input**: User presses `/`
- **Expected**: `/` appears in input. `drawer_open_ = true`. Drawer renders all top-level commands sorted: each line shows `name  args  help`. If a command has `current_value`, it is displayed: `name (current: X)  help`.
- **On failure**: No drawer or stale drawer content.

#### [SE-02] Narrow filter as user types (command level)

- **Given**: User has typed `/se`
- **Input**: User presses `t`
- **Expected**: Drawer filters to top-level commands whose name or alias starts with `set`: drawer shows `/set` with its usage and help. Partially typed prefix `/se` is shadow-completed in input if unambiguous.
- **Behaviour**: Filter returns three ordered buckets: exact name, exact alias, prefix name, prefix alias.

#### [SE-03] Tab complete to common prefix (shadow)

- **Given**: User has typed `/se`, drawer shows `set`, `session`
- **Input**: Tab press
- **Expected**: Input extends to `/set` (common prefix). No trailing space added. Drawer remains open showing `session` and `set`. Shadow completion never suggests invalid commands.

#### [SE-04] Tab complete to unambiguous match

- **Given**: User has typed `/ses`
- **Input**: Tab press
- **Expected**: Input extends to `/session` + space (single exact match). Drawer closes. Cursor ready for argument.
- **On failure**: Tab inserts wrong command or no space.

#### [SE-05] Up/Down drawer navigation

- **Given**: Drawer open with filtered results, selection at index 0
- **Input**: Down arrow
- **Expected**: Selection moves to index 1. Input buffer updated to the selected command's full name. Drawer re-renders with highlight on selected item. Wraps at bottom to top.
- **Edge case**: Empty drawer (no filtered results) — navigation is a no-op.

#### [SE-06] `?` key — show remaining options (Cisco IOS style)

- **Given**: User has typed `/set ` (argument mode, drawer open showing categories)
- **Input**: `?` key
- **Expected**: A popup or drawer section lists all valid next tokens. For each: the token name, description, and current value (if a setting). E.g.:
  ```
  detection   Loop/duplicate detection settings  (current: loop=off, duplicate=off)
  display     Display rendering settings         (current: markdown=on)
  toolfold    Tool result folding mode           (current: auto)
  policy      Read/write/yolo mode               (current: write)
  ```
- **On failure**: `?` typed as literal character or no response.

#### [SE-07] `?` at deeper level — narrow options

- **Given**: User has typed `/set detection `
- **Input**: `?` key
- **Expected**: Popup shows only detection-related options:
  ```
  loop       Enable/disable tool loop detection     (current: off)
  duplicate  Enable/disable duplicate call detection (current: off)
  ```
- **On failure**: Shows all options (not narrowed).

#### [SE-08] `?` at value position — show valid values

- **Given**: User has typed `/set detection loop `
- **Input**: `?` key
- **Expected**: Popup shows valid values:
  ```
  on     Enable loop detection
  off    Disable loop detection
  toggle Flip the current state
  ```

#### [SE-09] Enter dispatches drawer selection

- **Given**: Drawer open, selection on `/session`
- **Input**: Enter
- **Expected**: `/session` is dispatched. Drawer closes. Input cleared.
- **Edge case**: If user typed `/sess` and Enter, the drawer selection is used, not the typed text.

#### [SE-10] Enter dispatches full command line

- **Given**: User has typed `/set detection loop on`
- **Input**: Enter
- **Expected**: `handle_slash()` extracts command path, resolves via tree walk, calls final handler. Input cleared.
- **On failure**: Unknown command path → status `"unknown command"` at the level that failed.

#### [SE-11] Deep command — drawer updates per depth level

- **Given**: User types `/set detection loop ` (with trailing space)
- **Input**: Each `/`, `s`, `e`, `t`, ` `, `d`, `e`, ... as typed
- **Expected**: 
  - `/` → drawer shows top-level commands
  - `/se` → drawer narrows to `set`, `session`
  - `/set ` → drawer shows first-level subcommands: `detection`, `display`, `toolfold`, `policy`, `provider`, `model`, `think`, `compression`
  - `/set de` → drawer narrows to `detection`
  - `/set detection ` → drawer shows second-level subcommands: `loop`, `duplicate`
  - `/set detection lo` → drawer narrows to `loop`
  - `/set detection loop ` → drawer shows valid values: `on`, `off`, `toggle`
- **On failure**: Drawer stays at previous level.

#### [SE-12] Deep command — shadow completion at each level

- **Given**: User types `/set detection l`
- **Input**: Tab
- **Expected**: Shadow completes to `/set detection loop ` (unique match at this depth). Not `/set detection loop on` — only completes the current token.
- **On failure**: Completes multiple levels at once or doesn't complete.

#### [SE-13] Drawer shows current value + description per item

- **Given**: User types `/set ` (argument mode)
- **Expected**: Each drawer item shows: `name  args  help  (current: X)`. E.g.:
  ```
  detection <loop|duplicate> <on|off|toggle>  Loop/duplicate settings  (loop: off, dup: off)
  display   markdown <on|off>                 Markdown rendering       (markdown: on)
  ```
- **On failure**: No current values, or stale values.

#### [SE-14] Unknown command at depth

- **Given**: User types `/set foo bar baz`
- **Input**: Enter
- **Expected**: Tree walk resolves `set` → looks for subcommand `foo` → not found → status error: `"unknown command: /set foo"`. Dispatch stops.
- **On failure**: Partial dispatch or crash.

#### [SE-15] Escape closes drawer; escape again cancels

- **Given**: Drawer open
- **Input**: Esc
- **Expected**: `drawer_open_ = false`. Drawer disappears. Input buffer retains typed text.
- **Input**: Esc again (drawer closed, agent streaming)
- **Expected**: `cancel_token.request()` fires. Agent stream stops.
- **On failure**: Esc behaviour reversed.

#### [SE-16] `/no` prefix — invert command (Cisco IOS style)

- **Given**: User types `/no set detection loop`
- **Input**: Enter
- **Expected**: Engine detects `/no` prefix. Strips `no`, resolves `/set detection loop`. Inverts the action: instead of setting loop `on`, sets it to the opposite. Equivalent to `/set detection loop toggle`. Status: `"detection loop: toggled (now: on)"`.
- **Behaviour**: `no` prefix works at ANY depth: `/no set detection` → inverts detection settings (disables both loop and duplicate). `/no session save` → deletes the session.
- **On failure**: `no` treated as unknown command.

#### [SE-17] Command abbreviation — unambiguous prefix accepted on Enter (Cisco IOS style)

- **Given**: Only one command starts with `/se`: `/session`
- **Input**: `/se`
- **Input**: Enter
- **Expected**: Engine resolves `/se` against command tree → unique match → `/session` dispatched. No Tab required.
- **Given**: Two commands start with `/se`: `/set` and `/session`
- **Input**: `/se`
- **Input**: Enter
- **Expected**: Ambiguous → drawer opens showing both. No dispatch.
- **On failure**: `/se` rejected as "unknown command" or first match dispatched.

#### [SE-18] Command abbreviation at depth

- **Given**: `/s` uniquely resolves to `/set`, `/set d` uniquely resolves to `/set detection`
- **Input**: `/s d`
- **Input**: Enter
- **Expected**: At each depth level, the partial prefix is resolved. Matches `/set detection`. Dispatched without Tab.
- **On failure**: One of the levels incorrectly resolved.

#### [SE-19] Ctrl-D — list choices without inserting

- **Given**: Input = `/se`
- **Input**: Ctrl-D
- **Expected**: Popup shows all completions at current position: `set`, `session`. Input NOT modified. Dismiss with Esc. No cycle state armed.
- **On failure**: Completes prefix like Tab.

#### [SE-20] Ctrl-R — reverse history search

- **Given**: Prompt history has 20 entries
- **Input**: Ctrl-R
- **Expected**: Status bar shows `bck-i-search: _`. Typing characters
  searches history incrementally. Ctrl-R jumps to previous match.
  Enter accepts match. Esc cancels.
- **On failure**: No search or stale history.

#### [SE-21] Shortcut alias — single letter

- **Given**: User types `/q`
- **Input**: Enter
- **Expected**: Tree walk resolves `q` via aliases → matches `quit`. `cmd_quit` dispatched.
- **On failure**: `q` treated as unknown command.

#### [SE-22] Shortcut alias — glued two-letter

- **Given**: User types `/sl`
- **Input**: Enter
- **Expected**: Tree walk: `s` not found in top-level names → check aliases → `s` matches `session` via alias. Then `l` not found in session subcommands → check aliases → `l` matches `list`. `/session list` dispatched.
- **On failure**: Ambiguous alias or wrong expansion.

#### [SE-23] Shortcut alias shown in drawer

- **Given**: User types `/` (drawer open)
- **Expected**: Each command shows its alias(es) in the drawer line:
  ```
  quit (q)        Exit the application
  save (s)        Save current session
  compress (c)    Trigger compression
  session         Session management
    list (sl)       List saved sessions
    save (ss)       Save current session
  ```

#### [SE-24] Shortcut alias in autosuggest shadow

- **Given**: User types `/`
- **Expected**: Shadow shows `help` (first command). Both name and alias are
  accepted. Shadow shows the full name, not the alias.
- **Given**: User types `/q`
- **Expected**: Shadow shows `quit` after accepting alias → `/quit `.
  Tab works the same as with full name.

#### [SE-25] Shortcut alias — Tab cycle includes aliases

- **Given**: User types `/`, cycle through matches
- **Expected**: Tab cycling works the same whether user typed full name or
  alias. The cycle state is keyed by resolved name, not input text.

#### [SE-26] Shortcut conflict — two commands share alias

- **Given**: Two commands have the same alias by mistake
- **Input**: Alias `s` matches both `save` and `session`
- **Expected**: The alias should never be added if it conflicts. If it does,
  the FIRST match in the tree wins. A warning is logged at registration time.
- **On failure**: Silent conflict causes unpredictable dispatch.

---

#### [SE-27] Contextual help row in drawer header

- **Given**: Drawer open, partially typed command
- **Expected**: Drawer header shows:
  `Tab/Up-Dn: cycle  ?/Ctrl-D: options  Enter: run  Esc: cancel`
  When `?` was pressed and popup visible:
  `?/Ctrl-D: remaining options shown  Esc: dismiss`
- **On failure**: No header or stale instructions.

---

### Vim `:`-inspired input editing (readline-style key bindings)

These are not vim modal editing — they are the standard readline/emacs-style
key bindings that vim also uses in its `:` command-line mode. They make the
input line feel as capable as a shell prompt.

| Binding | Action | Source |
|---------|--------|--------|
| `Ctrl-A` / `Home` | Move cursor to beginning of line | readline / vim `:` |
| `Ctrl-E` / `End` | Move cursor to end of line | readline / vim `:` |
| `Ctrl-B` / `Left` | Move cursor backward one character | readline / vim `:` |
| `Ctrl-F` / `Right` | Move cursor forward one character | readline / vim `:` |
| `Alt-B` / `Alt-Left` | Move cursor backward one word | readline |
| `Alt-F` / `Alt-Right` | Move cursor forward one word | readline |
| `Ctrl-W` | Delete word backward (back to previous space or `/`) | readline / vim `:` |
| `Alt-D` | Delete word forward | readline |
| `Ctrl-U` | Delete from cursor to beginning of line | readline / vim `:` |
| `Ctrl-K` | Delete from cursor to end of line | readline / vim `:` |
| `Ctrl-Y` | Yank (paste) the last deleted text at cursor | readline |
| `Ctrl-L` | Clear screen and redraw | readline |
| `Ctrl-T` | Transpose characters (swap cursor-1 and cursor) | readline |
| `Ctrl-_` | Undo last edit | readline / vim `:` |

**Personality check**: These are standard readline bindings familiar to anyone
who uses a shell. They are NOT vim modal mode (no `i`, `Esc`, `dd`, etc.).
They are basic text-editing affordances that an AI agent prompt line should
have. Adding them is low cost, high value — users expect them.

**Implementation note**: These only apply to the input line editing, not to
any modal vim mode. The agent remains in insert-always mode for prompts.

These bindings are NOT yet implemented in the current code. They need to be
added to the `Tui::run()` key dispatch in `tui/tui.cpp`.

---

### Cross-references

- **Depends on**: `tui/input-system/auto-complete.md`, `tui/input-system/nested-commands.md`, `tui/input-system/contextual-help.md`
- **Depended on by**: `tui/event-loop.md` (key dispatch), `tui/layout-engine.md` (drawer overlay positioning)
- **Test coverage**: `tests/tui_tests.cpp` — palette helpers. `completion/tests/completion_test.cpp` — standalone tree-based completion (unused by TUI).

### Known gaps

1. **Flat command struct (current)** — `Command` has no `subcommands` vector. Deep nesting uses `complete_arg` lambdas + string splitting. Must migrate to `CommandNode` tree model.
2. **No `?` key handling (current)** — `?` treated as literal printable. Must be intercepted as help trigger at any depth.
3. **No `no` prefix (current)** — `/no` prefix for command negation not implemented.
4. **No partial command acceptance (current)** — Tab required before Enter for unambiguous prefixes.
5. **No Ctrl-D or Ctrl-R (current)** — These key bindings not yet assigned.
6. **Standalone `completion/` library unused** — Implements proper tree with `ArgSpec`, `FlagSpec`, `Completer` with shadow. Migration target.
7. **Completer state not reset on all actions** — Typing after failed completion may leave stale cycle state.
8. **Drawer clipped when too many commands** — No scrollbar or paging in drawer for long lists.
9. **Shift-Tab terminal support** — Some terminals may not send distinct Shift-Tab. Need fallback key binding.
10. **Input editing key bindings** — Ctrl-A/E/B/F/W/U/K/Y/L/T/_, Alt-B/F/D are not yet implemented.
11. **Shadow rendering in input line** — The `A_DIM` autosuggest suffix requires canvas changes to render faded text after the cursor without it being part of the real input buffer.

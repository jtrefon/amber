## Spec: TUI Auto-Complete

### Purpose
Provide autosuggest-style completion that is **always visible** as you type
(fish/zsh autosuggest), not triggered by Tab. The "shadow" — the completion
hint — appears inline in the input line in a dimmed/half-visible style,
updating on every keystroke. Tab accepts the suggestion. This matches how an
AI agent should work: anticipate, don't wait for a keypress.

Tab then cycles through alternatives (zsh-style menu selection). Ctrl-D lists
all options without modifying input. Ctrl-R searches history.

### Core principle: shadow is always visible, Tab just accepts it

When you type `/se`, the input line shows:
```
/se|t        ← "t" is dimmed/faded — the shadow completion
```
The drawer shows `set`, `session`, `settings` with descriptions. Pressing Tab
accepts the shadow (`/set `). Pressing Tab again cycles to next match.

This is **not** "Tab to see shadow". The shadow is **always there**. Tab is
just the accept key.

### Inspiration
- **fish shell / zsh autosuggest**: completion hint appears faded inline as
  you type; Ctrl-F or right-arrow accepts the suggestion
- **zsh menu selection**: Tab cycles through alternatives inline
- **BitchX**: drawer context with descriptions and current values
- **Cisco IOS `?`**: remaining options at any depth

### Personality note
This is an AI agent tool, not a shell or IRC client. Borrow patterns that
reduce cognitive load (autosuggest, menu cycling, inline help), not features
that add complexity without agentic value (registers, ranges, substitution).
Every borrowed pattern must answer: "does this make the agent easier to
interact with?"

### Ownership
- **Source files**: `tui/palette.h`/`.cpp` (`CommandNode`, `Completer`, `palette::filter()`, `common_prefix()`), `tui/tui_input.cpp` (`build_commands()`), `tui/tui_render.cpp` (draw_drawer, shadow render)
- **Test files**: `tests/tui_tests.cpp` — Completer tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Every printable character (updates shadow), Tab (accept shadow / cycle), Shift-Tab (reverse cycle), Ctrl-D (list all), Ctrl-R (history search) |
| **Output** | Input line with dimmed shadow completion suffix, drawer with candidates |
| **Error states** | No matches → no shadow shown, drawer says "no matches" |
| **Thread safety** | Main thread only. |

### Invariants

1. **Shadow is always visible** — after every keystroke, the input line shows
   a dimmed/faded completion hint for the most likely match at the current depth.
   The shadow is rendered in a dim colour (e.g., `A_DIM` or a grey colour pair)
   so it is visually distinct from typed text.
2. **Tab accepts the current shadow** — pressing Tab replaces typed text with
   the shadow completion and arms the cycle state.
3. **Subsequent Tabs** cycle forward through alternative matches INLINE — the
   current match replaces the token, the draw highlights the current item.
4. **Shift-Tab** cycles backward through alternative matches.
5. **Any further printable character** resets the cycle state and recomputes
   a new shadow based on the updated input.
6. **Ctrl-D** at any position shows a popup listing ALL completions for the
   current position WITHOUT modifying input. Useful when you want to see the
   full set before deciding.
7. **Ctrl-R** starts incremental reverse history search.
8. **At value positions**, the shadow shows the most likely value (first
   choice). Tab accepts it; Tab again cycles to next valid value.
9. **When shadow is empty** (no match), no faded text is shown. Tab does nothing
   (terminal beep).
10. **Tab on empty input** opens drawer showing all top-level commands with
     first match as shadow (also faded in input).
11. **`?` without space** (e.g., `/set detection loop?`) triggers a quick
     inline popup of remaining options for the current position. The `?` is
     stripped from input after the popup.
12. **`?` with space** (e.g., `/set detection loop ?`) triggers the full help
     page for the path before `?` (see `contextual-help.md`).

---

### Scenarios

#### [AC-01] Shadow appears as you type — single command match

- **Given**: Only `session` starts with `/se`
- **Input**: `/se` typed
- **Expected**: Input line shows: `/se|ssion ` where `ssion ` is dimmed/faded.
  Drawer shows `/session` with usage and description. The shadow is the full
  command + trailing space (ready for arguments).
- **On failure**: No shadow, or Tab required to see it.

#### [AC-02] Shadow adapts as you type — narrows down

- **Given**: Commands `set`, `session`, `settings` exist
- **Input**: `/se` typed
- **Expected**: Shadow shows `/set` (common prefix). Drawer shows 3 matches
  with `set` highlighted.
- **Input**: User types `s` → `/ses`
- **Expected**: Shadow updates to `/session ` (unique match now). Drawer
  narrows to `session`. Shadow faded text: `ion `.
- **On failure**: Shadow stays at `/set` or doesn't update.

#### [AC-03] Tab accepts shadow

- **Given**: Shadow shows `/set` (faded `t` after `/se`)
- **Input**: Tab
- **Expected**: Input becomes `/set`. Shadow consumed. Cycle state armed at
  index 0 (`set`). Drawer still shows 3 matches. Next Tab cycles to index 1.
- **On failure**: Tab inserts `t` as typed character instead of accepting
  shadow.

#### [AC-04] Tab cycles through matches inline

- **Given**: Input = `/`, cycle armed, 8 top-level matches
- **Input**: Tab (accept shadow → `/help`)
- **Input**: Tab (cycle → `/set`)
- **Input**: Tab (cycle → `/session`)
- **Expected**: Each Tab replaces the current token inline. Drawer highlight
  follows. Wraps: after last match, goes back to first.
- **On failure**: Popup dialog instead of inline replacement.

#### [AC-05] Shift-Tab reverse cycle

- **Given**: Cycle at index 2 (`session`), 8 matches total
- **Input**: Shift-Tab
- **Expected**: Index → 1 (`set`). Input becomes `/set`. Drawer highlight
  moves to `set`.
- **On failure**: Forward cycle or no-op.

#### [AC-06] Typing a character resets cycle, recomputes shadow

- **Given**: Cycle at index 2 (`session`)
- **Input**: User types `x`
- **Expected**: Cycle reset. Input becomes `/sessionx`. Shadow recomputed:
  no command starts with `/sessionx` → no shadow. Drawer shows no matches.
- **On failure**: Stale cycle continues or shadow shows stale completion.

#### [AC-07] Enter confirms current cycle selection

- **Given**: Cycle at index 1 (`set`)
- **Input**: Enter
- **Expected**: `/set` is dispatched. Input cleared. Cycle reset.
- **On failure**: Original input dispatched (not the cycled match).

#### [AC-08] Shadow at value position

- **Given**: Input = `/set detection loop `
- **Expected**: Shadow shows `on` (first choice). Drawer shows `on`, `off`,
  `toggle` with descriptions.
- **Input**: Tab → accepts `on`. Tab again → cycles to `off`.
- **On failure**: Shadow not shown, or shows wrong value.

#### [AC-09] Shadow at nested depth

- **Given**: Input = `/set d`
- **Expected**: Shadow shows `/set detection` (faded `etection`). Drawer shows
  subcommands of `set` starting with `d`: `detection`.
- **On failure**: Shadow jumps to `/set detection loop` (two levels).

#### [AC-10] No match — no shadow

- **Given**: Input = `/zzzzz`
- **Expected**: No faded shadow text after cursor. Drawer shows "no matches".
  Tab does nothing (terminal beep/flash for feedback).
- **On failure**: Crash or stale shadow from previous match.

#### [AC-11] Tab on empty input

- **Given**: Input buffer empty
- **Input**: Tab
- **Expected**: Shadow shows `help` (first command alphabetically). Drawer
  opens showing all top-level commands.
- **On failure**: Tab does nothing.

#### [AC-12] Ctrl-D — list all choices without modifying input

- **Given**: Input = `/se`
- **Input**: Ctrl-D
- **Expected**: Popup shows `set`, `session`, `settings`. Input NOT modified.
  No cycle armed. After dismiss, user continues typing.
- **On failure**: Completes prefix (like Tab) or modifies input.

#### [AC-13] Ctrl-R — reverse history search

- **Given**: History has 20 entries
- **Input**: Ctrl-R
- **Expected**: Status bar shows `bck-i-search: _`. As user types, history
  is searched incrementally from most recent backward. Ctrl-R jumps to
  previous match. Enter accepts match into input line. Esc cancels (restores
  input to before Ctrl-R).
- **On failure**: No match, or search not incremental.

#### [AC-14] Ctrl-R with no matches

- **Given**: History empty
- **Input**: Ctrl-R + `zzzzz`
- **Expected**: `failing bck-i-search: zzzzz`. Enter cancels. Esc cancels.
- **On failure**: Crash or garbage.

#### [AC-15] Arrow keys during inline cycle

- **Given**: Cycle armed with 8 matches
- **Input**: Down arrow
- **Expected**: Same as Tab — cycle to next match. Input updated. Drawer
  follows.
- **Input**: Up arrow
- **Expected**: Same as Shift-Tab — cycle to previous match.
- **Rationale**: Drawer is showing matches; Up/Down should navigate them.
  Eliminates confusion between "drawer nav" and "Tab cycling" — same action.

#### [AC-16] Shadow respects depth — one level at a time

- **Given**: Input = `/set d`
- **Expected**: Shadow = `/set detection` (only completes the current partial
  token `d` → `detection`). NOT `/set detection loop on`.
- **Input**: Tab → accepts `/set detection `. Shadow recomputed for next
  level: shows `loop`.
- **Input**: Tab → accepts `loop `. Shadow now shows `on`.
- **Invariant**: Shadow never completes more than ONE token level ahead.
- **On failure**: Shadow jumps multiple levels.

#### [AC-17] Shadow in key-chain mode

- **Given**: `config` has `allow_key_chain = true` with no known subkeys
- **Input**: `/config ne`
- **Expected**: No fixed subcommand matches. Shadow shows nothing (key-chain
  mode has no known keys to autosuggest). Drawer shows: `<key>... <value>`.
- **On failure**: Shadow completes a non-existent subcommand.

#### [AC-18] `?` without space — inline remaining options popup

- **Given**: Input = `/set detection loop` (no trailing space)
- **Input**: User types `?` immediately after `loop` (no space) → `/set detection loop?`
- **Expected**: Engine detects `?` without preceding space: quick popup of
  remaining options for the current position. Shows:
  ```
  on      Enable loop detection
  off     Disable loop detection
  toggle  Flip the current state
  ```
  The `?` is stripped from input → input becomes `/set detection loop `.
  Dismiss with Esc.
- **On failure**: `?` treated as literal character (passes "loop?" as value).

#### [AC-19] `?` without space at command root — show all commands

- **Given**: Input = `/` (just the slash, no text after it)
- **Input**: User types `?` → `/?`
- **Expected**: Quick popup showing all top-level commands.
  The `?` is stripped → input becomes `/`. Dismiss with Esc.

#### [AC-20] File-path completion (adopting shell standard)

- **Given**: Command declares `ArgSpec{Path}` (e.g., `/save myfile`)
- **Input**: Tab at the path argument position
- **Expected**: Standard shell behaviour:
  1. Tab completes the current path component
  2. Tab again cycles through directory/file name matches
  3. `foo/` completes as directory; `foo` without `/` is a file prefix
  4. Hidden files (`.name`) excluded unless typed prefix starts with `.`
  5. Relative paths against workspace root; absolute paths preserved
  6. Drawer shows completions with file type indicators
  7. `?` without space at this position shows available files popup
- **Rationale**: This is the standard established by bash/zsh. Adopting it
  ensures intuitive behaviour for anyone who uses a shell.

#### [AC-21] Enter-partial-acceptance (Cisco IOS) — shadow accepted implicitly

- **Given**: Shadow shows `/session ` (unique match)
- **Input**: Enter (without pressing Tab first)
- **Expected**: The shadow is treated as accepted. `/session` dispatched.
  No Tab required before Enter for unambiguous prefixes.
- **On failure**: Only typed text dispatched (missing the shadow suffix).

---

### Cross-references

- **Depends on**: `tui/input-system/slash-engine.md`, `tui/input-system/nested-commands.md`, `tui/input-system/contextual-help.md`
- **Depended on by**: `tui/event-loop.md`
- **Test coverage**: `tests/tui_tests.cpp` — basic Completer tests. Needs full autosuggest, cycle, Ctrl-D, Ctrl-R tests.

### Known gaps

1. **File-path completion not yet implemented** — Spec'd (AC-20), implementation needed.
2. **Shadow rendering in input line** — Need `A_DIM` colour pair for faded text. Must not interfere with cursor positioning (shadow is after cursor, not part of real input).
3. **Performance on large command trees** — Shadow recomputed on every keystroke. Must be fast (<1ms) for acceptable typing latency.
4. **Shift-Tab terminal support** — Some terminals don't send distinct Shift-Tab. Need fallback (Ctrl-P for reverse cycle?).
5. **Ctrl-R on large history** — Needs incremental search index for 10,000+ entries.
6. **Enter-partial-acceptance vs Enter to execute** — If Enter accepts shadow, how does user execute a partial command that they DON'T want shadow-completed? The shadow only appears when there's a match; if user doesn't want it, they can type a space or character to break the match.

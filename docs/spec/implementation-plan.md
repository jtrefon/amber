# Phase 2 Implementation Proposal — UX Overhaul

**Sign-off required before implementation begins.**

---

## Overview

Phase 2 delivers the complete command input experience: autosuggest shadow,
inline `?` help (dual mode), readline key bindings, Tab cycling, Ctrl-D/R,
shortcut aliases, file-path completion, and the expanded command tree with
full CRUD operations for providers, models, sessions, jobs, files, and
system operations.

Approximately 15 gaps closed across ~12 files, zero new dependencies.

---

## Implementation order (dependency-aware)

Each step builds on the previous. Tests are written BEFORE production code
(Red → Green).

```
Step 1:  Wire completion/ library    (prerequisite for tree model)
Step 2:  CommandNode tree model      (prerequisite for per-depth everything)
Step 3:  Shortcut alias resolution   (depends on tree model)
Step 4:  Readline key bindings       (independent, touches tui.cpp event loop)
Step 5:  Autosuggest shadow          (depends on tree model + readline)
Step 6:  Tab inline cycling          (depends on autosuggest)
Step 7:  Ctrl-D / Ctrl-R             (independent, keyboard shortcuts)
Step 8:  Up/Down arbitration         (depends on Tab cycling)
Step 9:  `?` dual-mode interception  (depends on tree model)
Step 10: File-path completion        (depends on Tab cycling)
Step 11: Expanded CRUD handlers      (independent, new command handlers)
Step 12: `/files` commands            (depends on Step 11 pattern)
Step 13: `/system` commands           (depends on Step 11 pattern)
```

---

## Step-by-step file changes

### Step 1 — Wire `completion/` library into TUI palette

**Files touched:** `tui/palette.h`, `tui/palette.cpp`, `completion/include/completion/command.h`, `Makefile`

**What:**
- The standalone `completion/` library has `CommandNode`, `ArgSpec`, `FlagSpec`,
  and a tree-based `Completer` with shadow support.
- Currently compiled as a separate build target but NOT linked into the TUI.
- Step 1 adds the include path and links `completion/lib/*.o` into `amber-tui`.
- No code changes to `completion/` itself — just build system wiring.

**Test:** `make test` still passes. `completion/tests/completion_test.cpp` compiles and passes.

**Risk:** Low. The library exists and compiles. Wiring is Makefile-only.

---

### Step 2 — `CommandNode` tree model

**Files touched:** `tui/palette.h`, `tui/palette.cpp`, `tui/tui_input.cpp`
(`build_commands()`), `tui/tui.h` (member types)

**What:**
- Replace the flat `struct Command` with a tree model:

```cpp
// New in palette.h (alongside existing Command for backward compat,
// or replacing it entirely — recommend replace for clarity)

struct ArgSpec {
    std::string name;
    enum Type { String, Choice, Integer, Float, Bool, Path } type = String;
    std::vector<std::string> choices;
    std::pair<double,double> range = {0, 0};
    std::string placeholder;
    std::string description;
    bool required = true;
};

struct CommandNode {
    std::string name;
    std::vector<std::string> aliases;
    std::string description;
    std::vector<CommandNode> subcommands;
    std::vector<ArgSpec> args;
    std::function<std::string()> current_value = nullptr;
    std::function<void(const ParsedCommand&)> run;

    // Key-chain mode
    bool allow_key_chain = false;
    int  min_keys = 0;
    ArgSpec value_spec;
};
```

- Migrate `build_commands()` from flat vector to tree hierarchy.
- Keep the existing `Command` struct for the transition period OR migrate all
  at once. Recommendation: replace in one commit since `commands()` is only
  used internally.
- Update `filter()` to walk the tree instead of flat list.
- Update `find()` to walk subcommands recursively.

**Test:** All palette tests pass. `build_commands()` produces equivalent
command tree. `/help` lists same commands.

**Risk:** Medium. This is the largest structural change. All palette functions
that operate on `std::vector<Command>` must be updated.

---

### Step 3 — Shortcut alias resolution

**Files touched:** `tui/palette.cpp` (`find()`, `filter()` recursion),
`tui/tui_input.cpp` (`build_commands()` — add alias entries)

**What:**
- Alias resolution is already partially supported (`Command::aliases`).
- Step 3 adds the BitchX/IrcII alias table from `nested-commands.md`:
  `/q` → quit, `/sl` → session list, `/ss` → session save, etc.
- The tree walker in `find()` already checks aliases at each level — this
  just adds the alias strings to `build_commands()`.
- Alias conflict detection: `build_commands()` logs a warning if two commands
  share the same alias.

**Test:** `/q` dispatches quit. `/sl` dispatches session list. `/ss`
dispatches session save. Alias shown in drawer.

**Risk:** Low. Data-only change (adding alias strings).

---

### Step 4 — Readline key bindings

**Files touched:** `tui/tui.cpp` (`Tui::run()` key dispatch)

**What:**
Add the following key handlers to the event loop, between printable-char
handling and Enter:

| Binding | Action | Implementation |
|---------|--------|----------------|
| `Ctrl-A` (1) | Move cursor to start | `input_cursor_ = 0` |
| `Ctrl-E` (5) | Move cursor to end | `input_cursor_ = input.size()` |
| `Ctrl-B` (2) | Backward char | `if (cursor > 0) --cursor` |
| `Ctrl-F` (6) | Forward char | `if (cursor < size) ++cursor` |
| `Ctrl-W` (23) | Delete word backward | Find last space or `/` before cursor, erase range |
| `Alt-D` (ESC+`d`) | Delete word forward | Find next space after cursor, erase range |
| `Ctrl-U` (21) | Delete to start | `input.erase(0, cursor)`; `cursor = 0` |
| `Ctrl-K` (11) | Delete to end | `input.erase(cursor)` |
| `Ctrl-Y` (25) | Yank | Insert `kill_buffer_` at cursor |
| `Ctrl-L` (12) | Clear/redraw | `redraw(input)` |
| `Ctrl-T` (20) | Transpose | Swap chars at cursor-1 and cursor |
| `Ctrl-_` (31) | Undo | Swap with `undo_buffer_` |
| `Alt-B` (ESC+`b`) | Backward word | Move cursor to start of current/previous word |
| `Alt-F` (ESC+`f`) | Forward word | Move cursor to end of current/next word |

**Implementation note:** Most of these are 1-3 line additions. The undo
buffer stores `(input, cursor)` pairs. Delete commands push to `kill_buffer_`.

**Test:** Each binding tested manually. No regressions in existing key
handling (Enter, Esc, Tab, Up/Down, printable chars).

**Risk:** Low. Individual key cases, no structural changes.

---

### Step 5 — Autosuggest shadow

**Files touched:** `tui/tui_render.cpp` (`draw_input()`), `tui/palette.cpp`
(new `shadow()` function), `tui/tui.h` (shadow state), `tui/widgets.h`
(confirm `P_GRAY` or similar dim pair exists)

**What:**
- Add `palette::shadow(input, tree)` that returns the shadow suffix string
  (the faded completion hint after the cursor).
- Shadow is recomputed on every keystroke (after `draw_input()` is called).
- In `draw_input()`: after rendering the real input text, render the shadow
  suffix in a dimmed colour (e.g., `COLOR_PAIR(P_GRAY)` or `A_DIM`).
- The shadow is NOT part of `input` — it's rendered purely for display.
- When input cursor is at the end AND a match exists → show shadow.
- When user starts typing something that doesn't match → shadow disappears.
- Tab accepts the shadow (moves faded text into real input).

**Key invariant:** The shadow is rendered AFTER the cursor, not as part of
the input string. Cursor position is unaffected.

**Test:** Typing `/se` shows `/se`t in dimmed text. Tab accepts → `/set `.
Typing `/zz` shows no shadow.

**Risk:** Medium. Requires careful ncurses rendering after cursor. Must not
interfere with cursor positioning in `draw_input()`.

---

### Step 6 — Tab inline cycling

**Files touched:** `tui/palette.cpp` (`Completer::handle_tab()`),
`tui/tui.cpp` (Tab handling in `Tui::run()`)

**What:**
- Replace the current multi-tab popup with inline menu cycling:
  - First Tab: accept shadow (replace partial token with full match).
  - Second Tab: cycle to next match inline (replace current token in input).
  - Shift-Tab: cycle backward.
- Remove `show_popup` path from `TabResult` (popup is now Ctrl-D only).
- Add `cycle_index_` and `cycle_matches_` state to `Completer`.
- `cycle_index_` is synced with `drawer_sel_` (shared state).

**Drawer sync:** The drawer highlights the current cycle index. When the user
navigates the drawer with Up/Down, the cycle index follows.

**Test:** `/se` + Tab → `/set`. Tab again → `/session`. Tab again →
`/settings`. Shift-Tab reverses. Drawer highlight follows.

**Risk:** Medium. The existing multi-tab popup logic is replaced. Tests must
verify cycle state resets on printable characters.

---

### Step 7 — Ctrl-D list choices / Ctrl-R history search

**Files touched:** `tui/tui.cpp` (key dispatch), `tui/menu_select.cpp`
(reuse for Ctrl-D), `tui/tui_input.cpp` (history search state)

**What:**
- **Ctrl-D:** Show all completions for current position in a popup
  (`menu_select`). Input NOT modified. User selects or dismisses with Esc.
- **Ctrl-R:** Enter "reverse-i-search" mode. Status bar shows
  `bck-i-search: _`. Typing characters searches prompt history incrementally.
  Ctrl-R jumps to previous match. Enter accepts. Esc cancels.
- History search needs: `history_search_buffer_`, `history_search_pos_`,
  `history_search_query_` state on `Tui`.

**Test:** Ctrl-D at `/se` shows `set, session, settings` in popup. Ctrl-R
searches history incrementally.

**Risk:** Medium. Ctrl-R is the most complex — it temporarily changes the
input mode and needs modal state on `Tui`.

---

### Step 8 — Up/Down arbitration

**Files touched:** `tui/tui.cpp` (Up/Down handling at lines 596-605 and
653-666)

**What:**
Replace the current two separate Up/Down blocks (one for drawer, one for
history) with a single arbitration:

```
if ch is Up/Down:
    if drawer is open AND has matches:
        cycle through matches (same as Tab/Shift-Tab)
    elif input is empty:
        navigate prompt history (current behaviour)
    elif input NOT empty AND drawer has NO matches:
        navigate history filtered by input prefix (zsh style)
    else:
        navigate history (fallback)
```

**Test:** Empty input → Up/Down = history. `/se` typed, drawer shows matches
→ Up/Down = cycle completions. `/se` typed, no matches → Up/Down = history
filtered by `/se`.

**Risk:** Low. Logic change only, no new state.

---

### Step 9 — `?` dual-mode interception

**Files touched:** `tui/tui.cpp` (key dispatch), `tui/palette.cpp` (help
resolution), `tui/info_dialog.cpp` (reuse for full help page)

**What:**
- When `?` is the last character typed:
  - **No space before `?`**: quick popup of remaining options for the
    current position (uses same completion data as Tab cycling).
    The `?` is stripped from input.
  - **Space before `?`**: resolve the path before the space, open full
    help page (`info_dialog`) showing usage, args, types, choices, current
    values, flags, related commands.
- Implementation: after every printable character, check if the input ends
  with `?` (with or without preceding space). If so, intercept.

**Key rule:** `?` is only intercepted when it's the LAST character of the
input. `/set loop?` triggers popup. `/set loop ?` triggers full page.
`/set loop?x` is normal typing (no interception).

**Test:** `/set detection loop?` → popup shows `on, off, toggle`.
`/set detection loop ?` → full help page. `/set val?` (no space, `?` is
literal) → dispatches normally.

**Risk:** Low. Pure key interception, no structural change.

---

### Step 10 — File-path completion

**Files touched:** `tui/palette.cpp` (new `complete_path()`),
`tui/tui_input.cpp` (commands declare `ArgSpec{Path}`)

**What:**
- When Tab is pressed at a position where the `ArgSpec` type is `Path`,
  use `complete_path()` to:
  1. Determine the base directory (from partial path or workspace root).
  2. List directory entries matching the partial filename.
  3. Return completions with trailing `/` for directories.
  4. Cycle through matches with Tab.
- Hidden files excluded unless the typed prefix starts with `.`.
- Drawer shows completions with file type indicators.

**This adopts the standard bash/zsh behaviour.** No novel design needed.

**Test:** `/save my` + Tab → completes to `/save myfile` (if `myfile` exists).
Tab again cycles to other `my*` files. `/save .` + Tab → shows hidden files.

**Risk:** Low. Standard pattern.

---

### Step 11 — Expanded CRUD handlers

**Files touched:** `tui/tui_input.cpp` (new handler functions)

**What:**
Add handler functions for the expanded command tree:

| Command | Handler | What it does |
|---------|---------|-------------|
| `/provider list` | `cmd_provider_list()` | List saved providers from `agent::list_saved_providers()` |
| `/provider add` | `cmd_provider_add(name)` | Add new provider (opens form_edit) |
| `/provider edit` | `cmd_provider_edit(name)` | Edit provider (opens form_edit) |
| `/provider delete` | `cmd_provider_delete(name)` | Delete provider with confirmation |
| `/provider test` | `cmd_provider_test(name)` | Test connection via `test_connection()` |
| `/model list` | `cmd_model_list()` | Fetch model list via `agent::list_models()`, show list_panel |
| `/model set` | `cmd_model_set(name)` | Set model, save global config |
| `/model probe` | `cmd_model_probe()` | Run `agent::probe_server()`, show results |
| `/session list` | `cmd_session_list()` | List sessions via `store_.list()` |
| `/session save` | `cmd_session_save(name)` | Save with optional name |
| `/session load` | `cmd_session_load(id)` | Load session from store |
| `/session delete` | `cmd_session_delete(id)` | Delete with confirmation |
| `/session rename` | `cmd_session_rename(id, title)` | Rename session |
| `/job list` | `cmd_job_list()` | List running jobs from `jobs_` service |
| `/job start` | `cmd_job_start(command)` | Start background job, return ID |
| `/job kill` | `cmd_job_kill(id)` | Kill job by ID |

**Test:** Each command tested in TUI. Expected output verified in scrollback.

**Risk:** Low. Each handler is independent and follows existing patterns
(`cmd_set`, `cmd_get`, `cmd_provider`, `cmd_model`).

---

### Step 12 — `/files` commands

**Files touched:** `tui/tui_input.cpp` (new handlers), `tui/files_panel.cpp` (new)

**What:**

| Command | Handler | What it does |
|---------|---------|-------------|
| `/files ls [path]` | `cmd_files_ls(path)` | List directory using `std::filesystem::directory_iterator`. Show name, size, modified, type. Color-coded. |
| `/files tree [path]` | `cmd_files_tree(path)` | Recursive directory tree using `std::filesystem::recursive_directory_iterator`. Indented with box-drawing chars. |
| `/files open <path>` | `cmd_files_open(path)` | Opens file in an embedded scrollable window (separate design — see below). **v1:** fallback to `append_line` with file content. |
| `/files find <pattern> [path]` | `cmd_files_find(pattern, path)` | Find files by name using `std::filesystem::recursive_directory_iterator` + glob match. |

**Design note for `/files open`:** v1 ships with inline content display
(file content appended to scrollback). The embedded scrollable window is a
separate design item.

**Test:** `/files ls` lists current directory. `/files tree` shows tree.
`/files find *.cpp` finds C++ files.

**Risk:** Low. Uses `std::filesystem` which is already a project dependency.

---

### Step 13 — `/system` commands

**Files touched:** `tui/tui_input.cpp` (new handlers)

**What:**

| Command | Handler | What it does |
|---------|---------|-------------|
| `/system exec <cmd>` | `cmd_system_exec(cmd)` | `popen()` or `std::system()`, capture stdout, append to scrollback. No stdin (v1). |
| `/system delete <path>` | `cmd_system_delete(path)` | `std::filesystem::remove()` for file, `remove_all()` for dir |
| `/system rmdir <path>` | `cmd_system_rmdir(path)` | `std::filesystem::remove_all()` |
| `/system mkdir <path>` | `cmd_system_mkdir(path)` | `std::filesystem::create_directories()` |
| `/system mv <src> <dst>` | `cmd_system_mv(src, dst)` | `std::filesystem::rename()` |
| `/system cp <src> <dst>` | `cmd_system_cp(src, dst)` | `std::filesystem::copy()` with `copy_options::recursive` |
| `/system info <path>` | `cmd_system_info(path)` | `std::filesystem::file_status`, permissions, size, modified time |
| `/system ps` | `cmd_system_ps()` | Read `/proc` entries, show PID/name/state/mem |
| `/system kill <pid>` | `cmd_system_kill(pid)` | `::kill(pid, SIGTERM)` |
| `/system df [path]` | `cmd_system_df(path)` | `statvfs()` — total, used, free, percentage |
| `/system uptime` | `cmd_system_uptime()` | Read `/proc/uptime` |
| `/system uname` | `cmd_system_uname()` | `uname()` syscall |

**Note on `exec`:** `popen(cmd.c_str(), "r")` with `fgets` loop. Output
captured and appended to scrollback. Optional: inject into agent context
(via `append_line` or a dedicated context buffer). Stdin not supported in v1.

**Test:** Each command tested in TUI. `/system exec ls` shows directory.
`/system ps` shows process list.

**Risk:** Low. Each command is a `popen()` or `std::filesystem` call.
Exception: `/system ps` reads `/proc` which is Linux-specific (already the
target platform).

---

## File change summary

| File | Steps | Type of change |
|------|-------|----------------|
| `tui/palette.h` | 2, 3, 5, 6, 10 | `Command` → `CommandNode`, new `shadow()`, `complete_path()` |
| `tui/palette.cpp` | 2, 3, 5, 6, 10 | Tree walk, shadow computation, inline cycling, path completion |
| `tui/tui.h` | 4, 5, 7, 8 | New members: `kill_buffer_`, `undo_buffer_`, `history_search_*`, shadow state |
| `tui/tui.cpp` | 4, 7, 8, 9 | Key dispatch: readline bindings, Ctrl-D/R, Up/Down arbitration, `?` interception |
| `tui/tui_input.cpp` | 3, 11, 12, 13 | `build_commands()` expanded tree + aliases; CRUD handlers; `/files`; `/system` |
| `tui/tui_render.cpp` | 5, 9 | `draw_input()` shadow rendering, drawer help update for `?` |
| `tui/tui_session.cpp` | 11 | Session CRUD handlers may use existing store methods |
| `Makefile` | 1 | Link `completion/lib/*.o` into `amber-tui` |
| `tui/files_panel.cpp` | 12 | NEW — file listing/tree rendering (optional, can inline in tui_input) |

**Estimated total:** ~12 files modified, ~800-1200 lines added/changed.

---

## Testing strategy

### Per-step tests (written before production code)

Each step includes:
1. Unit tests for new palette functions (`shadow()`, `complete_path()`,
   tree-walk `find()`, `filter()`)
2. TUI integration tests (for keyboard bindings that can be unit-tested)
3. Manual smoke test checklist

### Regression guard

After each step: `make clean && make && make test && make lint && make analyze`
must pass with zero new warnings.

### Manual smoke test (after all steps)

```
1. Type /se, see shadow "t", Tab accepts → /set
2. Tab again → /session, Tab again → /settings
3. Shift-Tab → /session, Shift-Tab → /set
4. Ctrl-D at /se shows popup with set, session, settings
5. Ctrl-R, type "set", Enter accepts from history
6. /set detection loop? → popup shows on/off/toggle
7. /set detection loop ? → full help page
8. Ctrl-A/E/B/F/W/U/K/Y/L/T/_ all work as expected
9. /q dispatches quit, /sl dispatches session list
10. /files ls shows directory, /files tree shows tree
11. /system exec echo hello shows "hello"
12. /system ps shows process list
```

---

## Risks and mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| `Command` → `CommandNode` migration breaks existing palette functions | Medium | Keep `Command` struct during transition, add `CommandNode` alongside. Migrate callers one at a time. |
| Autosuggest shadow interferes with cursor positioning | Low | Shadow is rendered after cursor using `mvaddnwstr` at cursor position + offset. Cursor position is unchanged. |
| Ctrl-R modal state conflicts with normal input | Low | `history_search_active_` flag gates all other key handlers while active. |
| `?` interception conflicts with `?` as part of command syntax | Low | Only intercept when `?` is last character. `/set val?` passes literal. `/set val ?` triggers help. |
| `/system ps` parsing `/proc` breaks on non-Linux | Low | Project targets Linux only (per VISION). Error message if `/proc` unavailable. |

---

## Acceptance

By signing off, you agree to the scope, order, and approach above.

- [ ] Step 1 — Wire `completion/` library
- [ ] Step 2 — `CommandNode` tree model
- [ ] Step 3 — Shortcut alias resolution
- [ ] Step 4 — Readline key bindings
- [ ] Step 5 — Autosuggest shadow
- [ ] Step 6 — Tab inline cycling
- [ ] Step 7 — Ctrl-D / Ctrl-R
- [ ] Step 8 — Up/Down arbitration
- [ ] Step 9 — `?` dual-mode interception
- [ ] Step 10 — File-path completion
- [ ] Step 11 — Expanded CRUD handlers
- [ ] Step 12 — `/files` commands
- [ ] Step 13 — `/system` commands

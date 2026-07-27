## Spec: TUI Layout Engine

### Purpose
Manage the spatial arrangement of chat scrollback, command input line, status
bar, and floating drawer/modal overlays in the ncurses window. All geometry
is recalculated every frame from `getmaxyx(stdscr)`.

### Ownership
- **Source files**: `tui/tui_render.cpp` (geometry helpers: `height()`, `width()`, `chat_top()`, `chat_height()`), `tui/canvas.h`/`.cpp` (windowed canvas for scrollback), `tui/window.h` (per-window state)
- **Test files**: No direct tests (ncurses-dependent). Canvas tests indirectly in `tests/tui_tests.cpp`.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Terminal dimensions from `getmaxyx(stdscr)` every frame. Window count (active + background). |
| **Output** | `(scrollback_y, scrollback_h)` for canvas, `(status_y)` for status bar, `(input_y)` for input line. |
| **Error states** | Terminal too small (<10 rows) → layout degrades (banner/clock clipped but functional). |
| **Invariants** | See below. |
| **Thread safety** | All geometry computed on main render thread. Read from agent thread never reads layout state. |

### Invariants

1. The status bar always occupies the second-to-last row (`height() - 2`).
2. The input line always occupies the last row (`height() - 1`).
3. The chat canvas fills all rows above the status bar.
4. The drawer overlay floats above the input line, starting at `chat_top()` and growing upward.
5. Terminal resize is detected every frame — no `SIGWINCH` handler.
6. Canvas is a separate ncurses `WINDOW*` (subwindow), recreated on resize.

---

### Scenarios

#### [LY-01] Standard layout (≥20 rows)

- **Given**: Terminal height ≥ 20, width ≥ 40
- **Expected**: Chat canvas fills rows 0..(h-3). Status bar at h-2. Input line at h-1. Canvas resize called every `draw()`.
- **On failure**: Layout overlaps or leaves gaps.

#### [LY-02] Very small terminal (<10 rows)

- **Given**: Terminal height = 8
- **Expected**: Chat canvas gets rows 0..5 (6 rows). Status bar at row 6. Input at row 7. No clipping — layout still functions.
- **On failure**: Negative height or crash.

#### [LY-03] Terminal resize — wider

- **Given**: Width expands 80→120
- **Expected**: `getmaxyx()` returns new size. `chat_canvas_.resize()` re-creates window. All committed lines re-wrapped to new width. Input line fills new width. Status bar adjusts.
- **On failure**: Gaps at right edge or lines clipped at old width.

#### [LY-04] Terminal resize — narrower

- **Given**: Width shrinks 120→80
- **Expected**: Same as expand but lines are re-wrapped to shorter width. Long lines wrap more.

#### [LY-05] Canvas resize — dimensions unchanged = no-op

- **Given**: No resize
- **Input**: `resize()` called with same y, h, w
- **Expected**: Early return — no `delwin`/`newwin`. `rewrap()` NOT called (content unchanged since last frame).
- **On failure**: Canvas re-created every frame (flicker).

#### [LY-06] Canvas rewrap on set_lines

- **Given**: New lines appended to scrollback
- **Input**: `set_lines(view)` called with updated content
- **Expected**: `rewrap()` called once. Each line word-wrapped to `cols_`. `is_hr` lines passed through (not wrapped).
- **On failure**: Lines not wrapped or wrong width.

#### [LY-07] Drawer overlay positioning

- **Given**: Drawer open, terminal height = 25
- **Input**: User types `/`
- **Expected**: Drawer renders at rows `chat_top()` upward, covering chat area. When drawer closes, chat area is restored on next `draw()`.
- **On failure**: Drawer overlays input line or status bar.

---

### Cross-references

- **Depends on**: `tui/event-loop.md` (draw calls geometry), `tui/scroll-system.md` (viewport within canvas)
- **Depended on by**: `tui/dialogs.md` (modal positioning), `tui/input-system/slash-engine.md` (drawer positioning)
- **Test coverage**: No direct layout tests.

### Known gaps

1. **No minimum size enforcement** — If terminal is too small (<10 rows), layout degrades rather than showing a "terminal too small" message.
2. **No `touchwin(stdscr)` after resize** — `wnoutrefresh` after resize may not repaint the full screen; `touchwin` is only called in `redraw()` and `redraw_after_modal()`.
3. **Canvas `rewrap()` called twice per frame** — Once in `append_line_ts()` (via `rich::wrap` for timestamp prefix) and again in `set_lines()` → `rewrap()`. Harmless but wasteful.

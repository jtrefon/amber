## Spec: TUI Scroll System

### Purpose
Manage the viewport over the chat scrollback: track scroll position,
auto-scroll to bottom on new content, handle page-up/down and line-by-line
scrolling via keyboard, and maintain scroll position across terminal resizes.

### Ownership
- **Source files**: `tui/window.h` (`scroll_top` member), `tui/canvas.h`/`.cpp` (`top_`, `max_top()`, `set_top()`, `render()` viewport), `tui/tui_render.cpp` (clamping, auto-scroll logic in `append_line()`)
- **Test files**: No direct scroll tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Keyboard navigation keys (PgUp/PgDn, Home/End, Alt+Up/Down, Alt+Left/Right). New content from agent events. |
| **Output** | `scroll_top` (index into wrapped line array of first visible row). `canvas.top_` mirrored from `scroll_top`. |
| **Error states** | Clamped to `[0, max_scroll]` always. |
| **Invariants** | See below. |
| **Thread safety** | `scroll_top` read/written from main thread only. |

### Invariants

1. `scroll_top` is always clamped to `[0, max_scroll()]`.
2. Auto-scroll to bottom triggers when `scroll_top >= max_scroll() - 2` (within 2 lines of bottom).
3. Auto-scroll does NOT trigger when user has scrolled up (viewport >2 lines from bottom).
4. Streaming tokens always auto-scroll to bottom (user is reading live output).
5. PgUp/PgDn scroll by `chat_height()` lines (one full page).
6. Terminal resize preserves scroll position relative to content (clamped to new `max_scroll`).

---

### Scenarios

#### [SC-01] Auto-scroll on new content

- **Given**: Viewport at bottom, new line appended
- **Input**: `append_line()` called
- **Expected**: If `scroll_top >= max_scroll() - 2`, sets `scroll_top = max_scroll()`. New content visible.
- **On failure**: Viewport stays at previous position, new content not visible.

#### [SC-02] Auto-scroll NOT triggered when scrolled up

- **Given**: User scrolled up to read history (`scroll_top < max - 2`)
- **Input**: New status line appended
- **Expected**: `scroll_top` unchanged. Viewport stays fixed at user's position.
- **On failure**: Viewport jumps to bottom.

#### [SC-03] Streaming tokens always auto-scroll

- **Given**: User has scrolled up, agent is streaming
- **Input**: `Token` event processed
- **Expected**: `scroll_top = max_scroll()` on every token. User cannot read history while streaming.
- **On failure**: Tokens append off-screen.

#### [SC-04] Page up/down

- **Given**: Scrollback has 100 wrapped rows, chat height = 20
- **Input**: PgUp pressed
- **Expected**: `scroll_top -= 20`. Clamped to 0. PgDown: `scroll_top += 20`. Clamped to max.
- **On failure**: Scroll position not clamped, or page size wrong.

#### [SC-05] Home/End

- **Given**: Any scroll position
- **Input**: Home pressed
- **Expected**: `scroll_top = 0` (top of scrollback).
- **Input**: End pressed
- **Expected**: `scroll_top = max_scroll()` (bottom).
- **On failure**: Scroll to wrong position.

#### [SC-06] Line-by-line scroll

- **Given**: Any scroll position
- **Input**: Alt+Up
- **Expected**: `scroll_top -= 1`. Clamped.
- **Input**: Alt+Down
- **Expected**: `scroll_top += 1`. Clamped.
- **On failure**: Jump by more than 1 line.

#### [SC-07] Resize with viewport scrolled up

- **Given**: Terminal 80×24, user scrolled up 10 lines
- **Input**: Terminal resized to 80×30
- **Expected**: `max_scroll()` increases. `scroll_top` clamped to new max if it exceeds it. If user was at a valid position, position is preserved.
- **On failure**: Viewport jumps to bottom or incorrect wrap lines up.

#### [SC-08] Scroll percentage display

- **Given**: `scroll_top = 50`, `max_scroll = 100`
- **Input**: Status bar computed
- **Expected**: Scroll percentage = `50%` displayed in status bar.
- **On failure**: Wrong percentage or division by zero when `max_scroll = 0`.

#### [SC-09] Max scroll when content fits

- **Given**: Scrollback shorter than chat height
- **Input**: `max_scroll()` computed
- **Expected**: `max_scroll = 0`. No scrolling possible. Scroll percentage hidden or `100%`.
- **On failure**: Negative max_scroll.

---

### Cross-references

- **Depends on**: `tui/layout-engine.md` (chat height), `tui/event-loop.md` (keyboard dispatch), `tui/canvas.md`
- **Depended on by**: `tui/event-loop.md` (agent event processing sets scroll_top)
- **Test coverage**: No direct tests.

### Known gaps

1. **No scrollbar widget** — Scroll position shown only as percentage text. No visual scrollbar.
2. **No search-in-scrollback** — No `/search` command or Ctrl+F for finding text in committed lines.
3. **Streaming overrides user scroll position** — Every `Token` event forces `scroll_top = max`. User cannot read history while streaming. Should be configurable.

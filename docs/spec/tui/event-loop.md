## Spec: TUI Event Loop

### Purpose
Drive the TUI at ~20 fps: poll ncurses for keyboard input, drain events from
the agent worker thread, assemble the scrollback view model, paint to the
terminal, and manage the command drawer, input line, and status bar.

### Ownership
- **Source files**: `tui/tui.cpp` (`Tui::run()`, `agent_worker()`, `drain_events()`), `tui/tui_render.cpp` (`draw()`, `draw_input()`, `draw_drawer()`, `draw_status_bar()`, `append_line()`, `append_markdown()`, `tick_clock()`, `trim_lines()`), `tui/canvas.cpp` (`resize()`, `set_lines()`, `rewrap()`, `render()`), `tui/window.h` (per-window state), `tui/textutil.cpp` (UTF-8 width, ANSI strip)
- **Test files**: No direct TUI loop tests (ncurses-dependent). TUI widget tests in `tests/tui_tests.cpp`.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Keyboard events from `getch()`, agent events from `AgentHooks` callbacks (queued under mutex), timer ticks (50ms poll interval) |
| **Output** | ncurses screen paint via `wnoutrefresh()` + single `doupdate()` per tick. Commands dispatched to agent. |
| **Error states** | Agent worker exception → `Error` event → status line. ncurses init failure → program exit. Modal deadlock → approval deferred to `pending_approvals_`. |
| **Invariants** | See below. |
| **Thread safety** | Agent thread writes to `event_queue_` under `event_mtx_`. Main thread reads in `drain_events()`. `agent_busy_`, `agent_cancel_` are `std::atomic<bool>`. |

### Invariants

1. `getch()` never blocks longer than 50ms (`timeout(50)`).
2. `doupdate()` is called exactly once per tick (coalesced via `dirty_` flag).
3. Every agent event is processed within 50ms of being queued.
4. Approval dialogs block the agent thread (via `std::promise`/`std::future`) but NOT the main thread.
5. Terminal resize is detected every frame via `getmaxyx(stdscr)` — no `SIGWINCH` handler.
6. The input line cursor is always repositioned after every draw.
7. Auto-scroll keeps the viewport at the bottom when new content arrives and the user hasn't scrolled up.
8. `pending_prompt_` preserves at most one queued message (last one wins).
9. The compress worker thread is detached and cannot be cancelled.

---

### Scenarios

#### [EL-01] Idle tick (no input, no events)

- **Given**: No key pressed, no agent events pending
- **Input**: `timeout(50)` expires → `getch()` returns `ERR`
- **Expected**: Every 150ms, `tick_clock()` redraws status bar (clock + activity wave). `draw_input()` repositions cursor. `doupdate()` flushes.
- **On failure**: Busy-spin at 100% CPU, or screen never updates.

#### [EL-02] Agent event arrives mid-tick

- **Given**: Agent worker pushes a `Token` event while main thread is in `getch()`
- **Input**: `Token{text: "Hello"}` queued under `event_mtx_`
- **Expected**: Next `ERR` return → `drain_events()` pops event → `draw()` called → stream buffer updates → `doupdate()` renders new text.
- **Latency**: ≤50ms from agent thread to screen.

#### [EL-03] Scrollback assembly: committed lines + stream buffer + reasoning

- **Given**: Window has 10 committed lines, 3 reasoning tokens, 5 stream tokens
- **Input**: `draw()` called during streaming
- **Expected**: `view` assembled as: `win().lines` (10) + wrapped reason lines (if `show_reasoning` and `!reason_folded`) + stream lines (rendered via `md::render()` if `markdown_on`). Canvas receives `view.is_code = false`, `view.heading = 0` for all non-markdown lines.
- **On failure**: Stream buffer or reasoning lines missing from view.

#### [EL-04] User types printable character

- **Given**: Input buffer empty
- **Input**: User presses `h`
- **Expected**: `ch = 'h'` → `input += 'h'` → `update_drawer(input)` → input line shows `"amber> h"` → cursor at position 8.
- **On failure**: Character not inserted, or inserted at wrong position.

#### [EL-05] User presses Enter — submit prompt

- **Given**: Input = `"read foo.txt"`
- **Input**: Enter pressed
- **Expected**: Prompt added to `win().prompt_history` (capped at 100). `send_async(prompt)` spawns agent worker. Input cleared. `pending_prompt_` cleared.
- **On failure**: Prompt not sent, or history not updated.

#### [EL-06] User presses Enter while agent busy

- **Given**: `agent_busy_ = true`, user types `"stop"`
- **Input**: Enter pressed
- **Expected**: `pending_prompt_ = "stop"`. Agent continues current turn. When done → `Done` event → `agent_busy_ = false` → `pending_prompt_` auto-sent.
- **On failure**: Prompt lost.

#### [EL-07] Esc closes drawer

- **Given**: Drawer open (user typed `/`)
- **Input**: Esc pressed
- **Expected**: `drawer_open_ = false`. Input buffer unchanged. No command executed.
- **On failure**: Drawer remains, or drawer content bleeds into scrollback.

#### [EL-08] Esc cancels streaming (drawer closed)

- **Given**: Drawer closed, agent streaming
- **Input**: Esc pressed
- **Expected**: `cfg_.cancel_token.request()` fires. HTTP transport aborts. Agent receives error, continues loop.
- **Double function**: Esc has different behaviour based on drawer state.

#### [EL-09] Approval dialog blocks agent, not UI

- **Given**: Agent needs approval for bash tool
- **Input**: `on_approval` hook creates `std::promise<Approval>`, queues event, blocks on `future.get()`
- **Expected**: Main thread drains `Approval` event → calls `resolve_approval()` → shows `menu_select()` dialog → user chooses → promise fulfilled → agent thread unblocks.
- **On failure**: Deadlock if main thread is blocked on a modal when approval arrives.

#### [EL-10] Approval deferred while another modal is open

- **Given**: Modal dialog already open
- **Input**: `Approval` event arrives
- **Expected**: Event handler checks `modal_open_` → pushes to `pending_approvals_` deque. After modal closes, `redraw_after_modal()` processes one pending approval.
- **On failure**: Nested ncurses dialog corruption.

#### [EL-11] Terminal resize

- **Given**: Terminal width increases from 80 to 120
- **Input**: `SIGWINCH` (no handler) → ncurses updates internal dimensions
- **Expected**: Next `getmaxyx(stdscr, h, w)` in `draw()` returns new size. `chat_canvas_.resize()` creates new `WINDOW*`. `rewrap()` re-wraps all lines to new width. Input line redrawn. Status bar fills new width.
- **On failure**: `touchwin(stdscr)` not called → partial repaint.

#### [EL-12] Auto-scroll on new content

- **Given**: Viewport at bottom (`scroll_top ≈ max_scroll`)
- **Input**: New line appended via `append_line()`
- **Expected**: `append_line()` checks `scroll_top >= max - 2` → sets `scroll_top = max`. New content visible.
- **User scrolls up**: If user scrolled up (`scroll_top < max - 2`), auto-scroll does NOT activate. Viewport stays fixed.

#### [EL-13] Scroll during streaming

- **Given**: User scrolls up to read history while agent streams
- **Input**: `KEY_PPAGE` (PgUp)
- **Expected**: `win().scroll_top` decremented by `lines_per_page()`. `scroll_top` clamped to `[0, max_scroll]`. New stream tokens accumulate in buffer but viewport stays fixed.
- **On failure**: Viewport jumps back to bottom on each new token.

#### [EL-14] Window switch during streaming

- **Given**: Multi-window TUI, window 1 streaming, user presses Alt+2
- **Input**: `Alt+2` → `active_win_ = 1`
- **Expected**: Window 2's scrollback displayed. Window 1 continues streaming in background (tokens accumulate in `win().stream_buf`). Switching back shows accumulated content.
- **On failure**: Data lost, or stream state corrupt across windows.

#### [EL-15] Large scrollback — trim_lines

- **Given**: `win().lines` exceeds 10,000
- **Input**: `trim_lines()` called in `draw()` before `set_lines()`
- **Expected**: Oldest lines removed from the front until `lines.size() <= max_lines_`. `scroll_top` adjusted to keep viewport stable.
- **On failure**: Memory growth unbounded.

#### [EL-16] Status bar — activity wave

- **Given**: Agent busy, `tick_clock()` called
- **Input**: `anim_phase_` increments every 150ms
- **Expected**: Wave pattern (`│ ▏▎▍▌▋▊▉█▉▊▋▌▍▎▏│`) renders in status bar. Right-justified clock updates.
- **On failure**: `anim_phase_` grows without bound (modulo handles display, but value grows forever).

#### [EL-17] Context gauge in status bar

- **Given**: `stats_.prompt_tokens` = 4000, context window = 8192
- **Input**: `draw_status_bar()` called
- **Expected**: Gauge shows `▐███░░░░░░░▌ 49% [4000/8192]`. Proportional to utilisation.
- **On failure**: Division by zero if `context_size == 0`, or negative percentage.

#### [EL-18] Signal handler — save and exit

- **Given**: `SIGHUP` or `SIGTERM` received
- **Input**: Signal handler runs
- **Expected**: `save_workspace_now()` persists all dirty windows. `_Exit(1)` terminates immediately.
- **On failure**: Signal handler is not async-signal-safe (string allocations, file I/O). Could deadlock.

---

### Cross-references

- **Depends on**: `tui/input-system/slash-engine.md` (drawer integration), `display/markdown-parser.md` (stream preview rendering), `tui/layout-engine.md` (canvas, panel hierarchy), `tui/scroll-system.md` (viewport management)
- **Depended on by**: `docs/spec/INDEX.md` (TUI category)
- **Test coverage**: No direct tests (ncurses-dependent). Indirect: `tests/tui_tests.cpp` — palette helpers, rich line wrapping, markdown rendering, canvas rewrap.

### Known gaps

1. **No SIGWINCH handler** — Relies on per-frame `getmaxyx()`. `wnoutrefresh()` after resize may not repaint full screen immediately; `touchwin(stdscr)` only called in `redraw()` and `redraw_after_modal()`.
2. **`compress_worker()` is detached** — Cannot be cancelled. If user quits during compression, thread accesses destroyed `Agent`.
3. **Large scrollback performance** — Full scrollback re-wrapped and re-rendered every frame (~50ms). For 10,000 lines × 3x wrap, this could exceed the frame budget.
4. **Double-wrapping in draw()** — Lines are wrapped in `append_line_ts()` (via `rich::wrap()` for timestamps) then re-wrapped in `canvas.set_lines() → rewrap()`. Harmless but wasteful.
5. **`anim_phase_` unbounded growth** — The modulo in the wave render handles display, but the integer value increments forever.
6. **Signal handler not async-signal-safe** — `save_workspace_now()` allocates strings and does file I/O. Could deadlock if signal arrives during malloc or write.
7. **`pending_prompt_` only remembers last** — If user types multiple prompts while agent is busy, only the last one is preserved.
8. **Window switch during streaming may surprise user** — Accumulated content bursts when switching back to a streaming window.

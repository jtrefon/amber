# Spec: chat surface and the one terminal adapter

- **Status:** 0.8. M6 of the ncurses shell plan.
- **Issue:** #518.
- **Related:** `render-foundation.md`, `dialogs.md`, `screen-host.md`.

## 1. TextCanvas

The chat scrollback is a `TextCanvas` of `rich::Line`, not a curses window. It
owns the source lines, the wrapped lines and the viewport offset; painting goes
into a `Surface` that the shell blits with the rest of the frame.

Wrapping is cached:

- `set_lines` compares the new view with the stored one. An identical view (the
  steady-state repaint every frame rebuilds) does not mark the wrap dirty.
- `set_viewport` invalidates only when the width changes. A height-only resize
  keeps the wrap.
- `ensure_wrapped` runs at most once per invalidation and increments
  `wrap_count()`; the test suite asserts that a scroll, or a resize followed by
  the same lines, performs exactly one wrap.
- The viewport is painted from the cached rows; a 10k-line transcript scrolls
  without touching the wrapper.

`rich::Run`/`Line` gained equality for the cache comparison; it is structural
(text, pair, emphasis flags), so a changed style is a new view.

## 2. FramePainter and the frame lifecycle

`RenderEngine` paints a persistent full-screen `Surface`:

- `begin_frame()` allocates the surface on the first paint or a terminal
  resize, and otherwise only re-binds the painter. The frame persists between
  `draw()` and `draw_input()`; each repaints its own regions.
- `FramePainter` is an ncurses-shaped façade over the surface: `move`, `attron`
  / `attroff`, `addnstr`, `mvaddnstr`, `addch`, `clrtoeol`, `mvhline`, and a
  `blit` for self-contained sub-surfaces (the canvas, the drawer). Text is
  clipped by **display columns**, so a UTF-8 glyph is never split; pair ids
  resolve through `role_for_pair` to semantic theme roles.
- `present_frame(bool update_cursor = true)` blits the frame into the terminal
  adapter's screen buffer. Only the input line asks it to place the physical
  cursor; full-frame draws and the pulse/clock repaints pass `false` so they
  never hide and re-show the cursor. A hide/show on every animation frame resets
  the terminal's blink timer, which reads as a cursor blinking at the paint
  cadence.

The adapter owns the only `doupdate()`. `RenderEngine::flush()` delegates to
`Terminal::flush()`, so a frame is virtual-screen-refreshed once and output
once, rather than per widget.

## 3. The adapter owns ncurses

`tools/ncurses_ratchet.py` now records exactly one file: `tui/terminal.cpp`.
Everything that used to reach into curses was moved or deleted:

| Concern | Where it lives now |
|---|---|
| initialisation, raw mode, keypad, ESC delay, colours, mouse | `Terminal::start` / `set_mouse` |
| key read, timeout probes, push-back, mouse decode | `Terminal::read_key*`, `push_key`, `read_mouse` |
| cursor, screen size, one output flush | `Terminal::place_cursor`, `size`, `flush` |
| key-code ABI assertions | `static_assert`s in `terminal.cpp` |
| legacy `Panel`/`Dialog` windows | deleted; no callers after the screen migration |
| `rich_paint` | deleted; `rich_surface` paints Markdown into cells |

The welcome mural keeps its exact xterm grayscale pairs through
`Style::exact_pair`, registered by the adapter; the banner uses the `Banner`
role (blue in Amber/Modern). `init_pairs()` is gone: the adapter registers the semantic role pairs and
the grayscale ramp once, after `start_color()`.

## 4. Behaviour preserved

The pty suite is unchanged and green: top rail, tabs, mouse, scroll mode, the
drawer/model picker, markdown and reasoning, working indicator, status bar,
input editing, menu, modals/approvals, skills and sessions. The frame is now one
surface, so a repaint can no longer leave a widget's attributes on the input
line (the adapter resets attributes after a blit).

## 5. Tests

- `tests/text_canvas_test.cpp` — wrap cache, width-only invalidation, content
  changes, viewport painting, wide glyphs, 10k lines scrolling without a wrap.
- `tests/frame_painter_test.cpp` — display-column clipping, pair/attribute
  mapping, `clrtoeol`, wide-text conversion and surface blits.
- `tests/tui_pty_test.cpp` — the full shell end to end.
- `make ncurses` — one allowed file, no exceptions.

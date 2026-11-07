# Spec: render foundation — cell grid, theme and components

- **Status:** 0.8. M1 of the ncurses shell plan (`docs/plan/tui-shell.md`).
- **Issue:** #513.
- **Scope:** `tui/` foundation only. No screen migration, no palette release.

## 1. Why a cell grid

Every screen used to paint straight into ncurses: wrapping, clipping, wide
glyphs, focus and hit testing were re-derived inside each widget, so a test
could only ask "did the right string reach the terminal?" by driving a pty. The
foundation inverts that: screens paint values into a `Surface`, and one terminal
adapter blits the difference. Layout, glyph width, clipping and composition are
pure functions over cells, so they are asserted headlessly.

The dependency direction is the point:

```text
screen/component -> Surface -> Cell          (pure, testable)
                    |
              terminal adapter -> ncurses   (one file)
```

## 2. Cell

A `Cell` is a grapheme string plus a `Style` (semantic role and attributes) and
a `continuation` flag. The string form lets a combining mark attach to the glyph
it modifies. The wide-glyph invariant is owned by `Surface`:

- a double-width glyph lives in its lead cell and is followed by exactly one
  continuation cell whose glyph is empty;
- overwriting either half blanks the other, so a half glyph can never survive;
- `put` refuses to place a wide glyph in the last column instead of splitting
  it.

`glyph_width(codepoint)` is a pure range table: 0 for combining marks, 2 for the
East Asian wide and emoji ranges, 1 otherwise. It is deliberately locale-free so
the gallery, the tests and the terminal agree; `wcwidth` is used only by the
legacy `rich_paint` path.

## 3. Surface

`Surface(width, height)` owns two buffers.

| Operation | Contract |
|---|---|
| `clear(cell)` | reset the whole back buffer |
| `put(x, y, cell)` | clip to bounds, repair a wide pair, write the glyph and its continuation |
| `put_text(x, y, text, style)` | UTF-8 decode, skip a glyph that cannot fit whole, return the columns placed |
| `fill(rect, cell)` | clipped fill through `put`, so wide pairs stay valid |
| `draw_box` / `draw_hline` | border primitives from a `Glyphs` set |
| `at(x, y)` | the painted (back) cell |
| `at_front(x, y)` | the cell the terminal currently shows |
| `row_text(y)` | one painted row's glyphs, continuations omitted (tests/debugging) |
| `diff()` | row-major changed cells; a stale continuation becomes a blank |
| `commit()` | swap front and back after a successful blit |

The diff rule for a replaced wide glyph has two entries: the new cell at the
lead and a blank where the old second half was. Without the second entry the
terminal keeps half of the old glyph on screen. A changed continuation whose
lead did not change is not emitted.

## 4. Compositor

A layer is a source `Surface`, a destination `Rect` and a z value; `composite`
paints layers in ascending z, insertion order breaking ties, clipped to the
target. Clipping never copies half a wide glyph: a continuation whose lead is
outside the area is skipped, and a lead whose continuation would be clipped is
skipped. A clipped edge therefore shows the layer underneath, not a broken glyph.

## 5. Theme and glyphs

`Role` is semantic: `Surface`, `SurfaceAlt`, `Frame`, `FrameFocus`, `Title`,
`Text`, `Muted`, `Accent`, `Selected`, `Attention`, `Success`, `Error`,
`Warning`, `Hint`, `Shadow`. A `RoleStyle` carries foreground/background
`Color` plus bold/dim/italic/underline/reverse. `Theme::amber(mode)` is the
shipped table; the glyph mode is the only variant today (full palettes are M7).

The terminal adapter resolves a role to a colour pair and clamps:

- 8 colours: no grey, so `DarkGray` falls back to white;
- 16 and 256: the same ANSI base palette is valid, so no lookup table is needed;
- no colour support: attributes only, pair 0.

`Glyphs` is two explicit sets, `unicode()` and `ascii()`: border corners and
tees, the strong rule, tab caps, scrollbar thumb/track, diamond, dot, cross,
attention, arrows, bullet, ellipsis and four spinner frames. Screens name a
glyph through the set; they never spell one inline. The ASCII set is what
`TERM`/locale detection cannot get wrong, and it is deterministic in tests.

## 6. Components

- `Component`: `measure(available)`, `arrange(rect)`, `paint(surface)`,
  `handle(event)`, `focusable()`.
- `FocusManager`: ordered components, `next`/`prev` skipping non-focusable
  entries, explicit `focus()`, and `dispatch()` to the focused component only.
- `HitMap`: rects in paint order, topmost-wins `at(x, y)`.

The base owns the bounds, nothing else. Layout containers and the modal stack
arrive with the milestones that need them (M4/M5); this milestone keeps the
surface small enough to be proven.

## 7. Gallery and dev screen

`render_gallery(size, mode)` paints every primitive: frame corners/edges, role
styles, the glyph set, a wide glyph, a selection strip, a scrollbar and the
state marks. `tests/render_foundation_test.cpp` asserts it at **80×24 and
120×40**, Unicode and ASCII:

- the title is present at both sizes;
- Unicode mode contains a continuation cell, ASCII mode contains none;
- every ASCII row is pure ASCII.

`amber --gallery` runs the same painter through the terminal adapter: `a` swaps
glyph modes, the surface repaints in place (so the diff path is exercised),
`Esc`/`q` quits, and the normal exit marker is written. A pty test drives that
path.

## 8. The ncurses ratchet

`tools/ncurses_ratchet.py` scans tracked (and untracked, non-ignored) code files
for a direct `ncurses`/`ncursesw`/`panel`/`form`/`menu` include and compares the
set with `tests/ncurses_baseline.json`.

- a file not on the list that includes one fails with the line number;
- a listed file that no longer includes one passes and is reported as ready to
  drop; `--update` records the smaller set;
- a missing baseline, an unreadable scan or a scan below the file floor exits 2,
  never 0.

The list started at the pre-foundation set plus `tui/terminal.cpp`, the adapter.
M6 retired the rest: `tests/ncurses_baseline.json` now names **only**
`tui/terminal.cpp` (`chat-surface.md`). The gate runs in `make static-gates`,
`make verify-quick` and the `ncurses` gate-report row.

## 9. Acceptance mapping

| Requirement | Where |
|---|---|
| a component paints into a cell grid, asserted without a TTY | `component_arranges_measures_and_paints_inside_its_bounds`, gallery tests |
| a surface diff blits to ncurses in one `doupdate()` | `Terminal::present` + the gallery pty test |
| gallery renders every primitive at 80×24 and 120×40, Unicode and ASCII | `gallery_renders_every_primitive_at_both_target_sizes` |
| wide glyphs, continuation cells and clipping | surface/compositor tests, including the compositor half-glyph cases |
| no new ncurses include outside the ratchet | `tests/gates/test_ncurses_ratchet.py`, `make ncurses` |

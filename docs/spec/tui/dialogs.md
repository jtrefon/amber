# Spec: dialogs and the modal stack

- **Status:** 0.9 target. The M4 stack (`ModalHost` + `DialogView`) is the
  contract; the visual layer in §4 is **not implemented** at `dc07c22b`.
- **Issue:** #516 (the modal stack). The visual regression and its acceptance
  checklist are tracked in #619.
- **Related:** `render-foundation.md`, `themes.md`, `architecture.md`,
  `docs/style/tui-design-guide.md`.

## 0. Current state (measured on `dc07c22b`, 80×24)

The stack works; the sheet does not. `paint_dialog` fills the body with
`SurfaceAlt` (default on dark gray), prints body text with `Text` (default on
the terminal's default background), and lays the four approval options in a
single horizontal row. At 80 columns that row is wider than the sheet:

```
┌──────────────────────────────Approve action?───────────────────────────────┐
│ run: rm -rf build/ (bash)                                                  │
│                                                                            │
[1] Allow once (60s)  [2] Allow session (60s)  [3] Always allow (60s)  [4] Alwa
│      1-4: pick   Tab/left/right: move   Enter: confirm   Esc: cancel       │
└────────────────────────────────────────────────────────────────────────────┘
```

- The first option starts at column 0, over the left border; the last is
  clipped before its label.
- `(60s)` is appended to **every** option, not to one status indicator.
- The title overwrites the top border without `┤`/`├`, on the default
  background.
- The footer is a muted line inside the body, not the bottom-border key bar.
- `Selected` is black on yellow, not the commander cyan.
- `Shadow` is black on black, so the one-cell drop shadow is invisible.
- The blue exists: `P_DIALOG` maps to `Banner` (white on blue), but the modal
  painter never uses it. `legacy_pairs_keep_the_bar_and_the_pulse_shades`
  asserts the pair, not the sheet, which is why the regression is green.

The full gap, repro and root-cause map are in #619. This spec describes what
the sheet must become.

## 1. Why one host

A dialog is not a screen: it blocks the agent worker that asked for an answer,
paints over the frame the user was reading, and returns one value. The old
widgets each owned an ncurses `WINDOW`/`PANEL`, a `getch()` loop and a modal
flag, so deferred approvals, Esc/cancel semantics and the input timeout were
re-derived per dialog. `ModalHost` owns exactly those concerns once:

| Concern | Owner |
|---|---|
| terminal overlay, keys, timeout | `ModalHost` + `Terminal` |
| what the sheet means / how it reacts | `dialog_state` (pure) |
| what it looks like | `dialog_view` (pure `Surface`) |
| deferring while a modal is open | `EventRouter` pending queues |
| the modal flag | `ModalScope` (`widgets.h`) |

The state machines take a key and return `DialogKey::{None,Redraw,Accept,Cancel}`;
the host blits `paint_dialog(view, size)` and never sees ncurses types. The form
reuses `form_focus`'s documented key map, so its key behaviour cannot drift.

## 2. Host contract

- **Blocking, as before.** A dialog runs on the UI thread and the agent worker
  waits on its promise. That is the existing approval/ask contract.
- **Overlay, not a full screen.** The host blits only the dialog surface at the
  centred origin. On close it restores `stdscr` with one full refresh, so the
  frame underneath returns.
- **Timeout restore.** The host sets the read timeout per iteration (250 ms
  while an approval counts down, blocking otherwise) and restores
  `kTickTimeoutMs` on every exit path, including cancel and timeout.
- **Active window is untouched.** No sheet reads or sets the active window. A
  background tab's approval names that tab (`[title] summary`) and leaves focus
  where the user was; the tab carries `!` while the question is pending.
- **Deferred questions.** `EventRouter` queues approvals, plugin asks and
  API-key requests while `modal_open_` is set and pumps one per tick afterwards.
  No nested curses dialog exists anymore.

## 3. Sheets

| Sheet | Constructor | Keys | Result |
|---|---|---|---|
| Confirm | title, message | Tab/arrows toggle, Enter accepts, Esc cancels | bool |
| Approval | summary, timeout, default | `1`-`4` pick+accept, Tab/arrows move, Enter accepts selection, Esc denies | `agent::Approval` |
| Info | title, rows | Up/Down/PgUp/PgDn/Home/End scroll, Enter/Esc/q close | none |
| List | title, items | Up/Down/PgUp/PgDn, Enter select, `/` filter, Esc cancel | index or -1 |
| Form | title, fields, notice | Tab/arrow field navigation, printable insert, Backspace/Delete, Enter accept, Esc cancel | bool + edited values |
| Custom sheet | a caller-owned `PaintedDialog` painter | `Accept`/`Cancel` from the handler; used by the skills install sheet and facet overlay | caller-defined |

### Delegated-worker approvals

A sub-agent inherits the parent's tool registry, so a gated tool inside a
worker must reach the window that owns the worker — not the active tab, and not
an empty hook. `worker_target.h` resolves the owner id through
`WindowManager::by_owner_id` (the same stable mapping watch reports use):

- **Owner active:** the dialog is addressed to that window.
- **Owner inactive:** the dialog goes to the active window with the owner named
  in the prompt (`worker [<owner title>]: worker "<label>": <summary>`), and
  focus does not move.
- **Owner gone, no window, or router shutting down:** the worker's hook returns
  `Deny` immediately; the child's gated tool fails closed with the normal
  no-approver reason. The active tab is never a substitute for a closed owner.

The worker blocks on the same promise as a foreground approval, so the UI thread
never waits. The executor records `blocked` and `awaiting approval: <tool>` on
its task while the dialog is up, then `approval: <verdict>`; `/tasks list` and
`/tasks show` include those worker rows. A blocked worker is live work for
`work_auto_attach`, so its viewer stays open. Headless runs install no factory
and keep `--yes`/no-TTY policy.

The approval countdown **denies on expiry**. It never auto-confirms: the
fail-safe is `ApprovalModel`'s invariant, not the painter's.

Paste into a form is decoded from the bracketed-paste stream before the key
path, so an API key can be pasted without the leading ESC cancelling the dialog.
Control bytes in a paste are dropped; the field stays single-line.

## 4. Visual contract

### 4.1 Target

A modal is a **blue commander sheet**: one blue surface from frame to body,
a white/cyan frame, the title inset in the top border (`┤ Title ├`), a visible
one-cell drop shadow, a key bar in the bottom border, and buttons that read as
filled blocks. This is the Norton Commander / Midnight Commander lineage the
top bar already follows; the sheet is not a gray ncurses `newwin`.

The sheet is a single background. The fill and body text share it; selection
bars, buttons, footer key chips and an error banner are deliberate islands with
their own background. What must not happen is a **body text** cell painted with
the generic `Text`/`Muted` roles, which carry the terminal default background
and punch black holes in the sheet.

### 4.2 Roles

New semantic roles, resolved by `Theme` exactly like the existing ones. The
names are the contract; the exact palette is `themes.md`.

| Role | Amber / Modern | High contrast | Monochrome | Used for |
|---|---|---|---|---|
| `DialogSurface` | White on Blue | Black on White | Default | body and border fill |
| `DialogText` | White on Blue | Black on White | Default | body text, field values |
| `DialogMuted` | Cyan on Blue | Black on White, dim | Default, dim | hints, labels, countdown |
| `DialogTitle` | White on Blue, bold | Black on White, bold | Default, bold | title text |
| `DialogFrame` | Cyan on Blue, bold | Black on White, bold | Default, bold | box, `┤ ├` |
| `DialogAccent` | Yellow on Blue | Black on White, bold | Default, bold | option numbers, brand |
| `DialogSelected` | Black on Cyan | White on Black | Default, reverse | selected option/row |
| `DialogButton` | Black on Cyan | Black on White | Default, reverse | idle button |
| `DialogButtonFocus` | Black on White | White on Black | Default, reverse, bold | focused button |
| `DialogFooterKey` | Black on Cyan | Black on White, bold | Default, reverse | `[Enter]` keys |
| `DialogFooterLabel` | Cyan on Blue | Black on White | Default | footer labels |
| `DialogError` | White on Red | Black on White, bold underline | Default, reverse, bold | notice banner |
| `Shadow` | DarkGray on Default, `░` | Black on Default, `░` | Default, dim, `░` | drop shadow |

`Shadow` is the one role that carries a glyph: a solid block that is black on a
black terminal is not a shadow. Unicode uses `░`; ASCII uses `:` (or `#` at
the adapter's choice), and the glyph is painted in the `Shadow` style. A theme
may make the shadow denser in 256-colour mode, but the role must remain
distinguishable from both `DialogSurface` and the default background.

`Selected` may also become black-on-cyan for list rows outside dialogs; that
is a follow-up, not a requirement here. The modal painter uses `DialogSelected`
so a global `Selected` change cannot silently restyle a sheet.

### 4.3 Geometry and sizing

- **Width:** content-driven — title, longest body line, longest option, footer —
  then clamped to `[40, min(80% of screen, screen.w - 2)]`. The approval sheet
  has a preferred width of **62** and must fit in **78** at 80 columns.
- **Height:** top border + body + button row (when present) + bottom border.
  Body padding is one column left and right. A blank row separates the summary
  from options and options from the countdown when that reads better.
- **Stability:** the width is computed from the *content*, never from the
  current countdown digits. Painting the same view at 60 s and at 9 s produces
  the same width, origin and option rows.
- **Centering:** `(screen.w - surface.w) / 2`, `(screen.h - surface.h) / 2`;
  the surface includes the one-cell shadow, so the frame is one cell up/left of
  the optical centre.
- **Fitting:** nothing paints outside `[0, width) × [0, height)`. A button row
  that would exceed the inner width stacks instead of clipping. The screen is
  never resized by a dialog; the host clamps and the painter respects it.
- **Minimum target:** 80×24. At that size the approval sheet shows the summary,
  all four options, the countdown and the full footer. 120×40 is the second
  target.

### 4.4 Frame and title

```
┌──┤ Approve action? ├──────────────────────────────────────────┐
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

- A single-line box from `Glyphs` (`┌ ┐ └ ┘ ─ │`; ASCII `+ - |`).
- The title is centred in the top border and is surrounded by `┤` and `├`,
  with the horizontal line continuing outside them. The title segment carries
  `DialogTitle`, the delimiters `DialogFrame`, and the rest of the border
  `DialogFrame`.
- The title is truncated with `…` if it cannot fit with two border cells on
  each side; it never overwrites the corners.
- The border background is the sheet background. A title that paints the
  terminal default behind itself is a bug.

### 4.5 Footer

The footer replaces the bottom border line; it is not a body row.

```
└─ [1-4] Pick  [Tab] Move  [Enter] Confirm  [Esc] Cancel ──────┘
```

- Segments are `[key] label`, two spaces between segments, with the corners
  preserved. Keys use `DialogFooterKey` (a filled chip); labels use
  `DialogFooterLabel`.
- The footer is one row. If the width is insufficient, drop segments from the
  left first and always keep `[Esc] Cancel`; never wrap into a second row.
- Every sheet has a footer. `Esc` is always the last segment.
- The old `view.footer` single string is not sufficient for styling keys;
  `DialogView` carries structured segments (or an equivalent that lets the
  painter style key and label separately).

### 4.6 Body, selection and buttons

- Body text uses `DialogText`; secondary text and field labels use
  `DialogMuted`; notices use `DialogError`.
- A selected option/row is a **full-width bar** from the inner left edge to the
  inner right edge, filled with `DialogSelected`; the number/key and label are
  painted on that bar. Highlighting only the glyphs is not a selection.
- A button is a filled block: `[ Label ]` with one cell of padding inside the
  brackets, one cell of blank between the bracket and the label, and two cells
  between buttons. Idle uses `DialogButton`; focused uses
  `DialogButtonFocus`. The row is centred and one cell above the footer.
- Form fields: the label is `DialogMuted`, the value `DialogText`, the focused
  value `DialogSelected`. The cursor is returned as a cell position and shown
  only while a field has focus.
- Scroll markers use `DialogMuted` in the right inner column, one at the top and
  one at the bottom when the content overflows.

### 4.7 Approval sheet

```
┌──┤ Approve action? ├────────────────────────────────────────────┐
│ [main] bash: rm -rf build/                                    │
│                                                                │
│  1  Allow once                                                 │
│  2  Allow session                                              │
│  3  Always allow                                               │
│  4  Always deny                                                │
│                                              auto-deny in 60s  │
└─ [1-4] Pick  [Tab] Move  [Enter] Confirm  [Esc] Cancel ───────┘
```

- The four options are **vertical**, one row each, all visible at 80×24. The
  selected row is a full-width `DialogSelected` bar; the `1`…`4` prefix uses
  `DialogAccent`.
- The summary is `[window] tool: args`, wrapped to the inner width, at most
  three lines; longer summaries end with `…`. It never scrolls horizontally.
- The countdown is one `auto-deny in Ns` line, right-aligned in `DialogMuted`,
  rendered only when `timeout_sec > 0`. It is **not** appended to an option
  label. The field reserves three digits, so 60 s and 9 s occupy the same
  width. The deadline denies; it never confirms the selected option.
- Width 62 preferred (range 58–78); height grows only for the summary. At 80×24
  the full sheet is at most 78 wide and 22 tall including the shadow.
- Keys: `1`–`4` select and accept immediately; Tab/Left/Right move; Enter
  accepts the selection; Esc denies.

### 4.8 Other sheets

Confirm:

```
┌──┤ Delete Session ├──────────────────────────────┐
│ Delete "Today's work"?                           │
│                                                  │
│            [ Yes ]       [ No ]                  │
└─ [Tab] Switch  [Enter] Confirm  [Esc] Cancel ────┘
```

- The safe choice (`No`) starts focused; the focused button is black on white.
- The message wraps; the width is 46–60 and the sheet is at most 8 rows.

Info: title, scrollable rows, scroll markers, footer in the border. No buttons.

List: selected row is a full-width `DialogSelected` bar; the filter strip is
`DialogMuted` with a `/` prefix; scroll markers; footer.

Form: fields and a centred `[ OK ]  [ Cancel ]` row; notice as a
`DialogError` banner; footer. Tab/arrow navigation, Enter accepts, Esc cancels.

Custom sheets (skills install, facets) paint their own content but must use the
same `paint_dialog` frame, title, shadow and footer; they do not draw their own
border or background.

### 4.9 Fallbacks

- **ASCII glyphs:** `+ - |` border, `:` shadow, no box-drawing or shade
  characters. The layout and roles are identical.
- **8 colours:** the blue surface, white text, cyan frame and black selection
  are all in the base eight; `DarkGray` falls back per `themes.md`. A shadow
  glyph keeps the drop shadow visible.
- **No colour / `NO_COLOR`:** attributes only. The sheet is distinguished by
  its border, the `┤ Title ├` delimiters, the shadow glyph, and bold/reverse
  for title and selection; no colour-only signal.
- **High contrast and monochrome:** per the role table. Losing hue never loses
  the hierarchy: frame bold, title bold, selection reverse, focused button
  reverse.

## 5. Invariants

1. The fill and body text share the sheet background; no body-text cell
   paints the terminal default background. Selection bars, buttons, footer
   chips and an error banner are the only islands.
2. The title has `┤`/`├` delimiters and the sheet background behind it.
3. The footer is the bottom border row, with a styled key for every action.
4. The drop shadow exists, is one cell right/bottom, and is visible as a role
   and glyph (or a contrasting block) on a black terminal.
5. Buttons are filled blocks, inside the frame, never clipped; an overflowing
   row stacks.
6. Selected options/rows are full-width bars, not text-only highlights.
7. The approval sheet shows all four options at 80×24; the countdown is one
   status line, never an option label; width is stable across ticks.
8. The sheet never paints outside the screen and never exceeds the declared
   bounds.
9. The frame, title, footer and shadow are shared by every sheet; a custom
   sheet may not invent its own chrome.

## 6. Tests

Add to `tests/dialog_test.cpp`:

- `dialog_uses_one_sheet_background`: every cell in the frame rect resolves to
  `DialogSurface` (or an allowed banner), never `Text`/`Muted`'s default
  background.
- `dialog_title_is_inset_between_frame_breaks`: row 0 contains `┤` and `├` and
  the title.
- `dialog_footer_is_the_bottom_border`: the bottom row contains `[Esc]` and
  `Cancel`; no footer row inside the body.
- `dialog_shadow_is_visible`: the right/bottom strip uses `Shadow` and a
  non-space glyph; its style differs from `DialogSurface`.
- `approval_options_are_vertical_and_all_visible_at_80x24`: four option rows,
  each label entirely inside the frame; no label overlaps the border.
- `approval_countdown_is_single_and_stable`: paint at 60 s and 9 s → identical
  width, origin and option rows; exactly one `auto-deny` string; no option
  contains `s)`.
- `approval_selected_row_is_a_full_width_bar`: every cell from the inner left
  to the inner right of the selected row is `DialogSelected`.
- `confirm_buttons_are_filled_blocks_and_fit`: both buttons fully inside the
  frame, separated by two cells, focused style on the safe choice.
- `all_sheets_fit_80x24_and_120x40`: confirm, approval, info, list and form;
  no cell outside the screen.

Extend `tests/render_foundation_test.cpp`:

- `dialog_roles_are_blue_and_distinct`: in Amber/Modern, `DialogSurface`,
  `DialogText`, `DialogFrame` and `DialogTitle` all carry `Blue`; `Shadow`
  differs from both the sheet and the default; `DialogSelected` is black on
  cyan; the legacy `P_DIALOG` assertion stays.
- `dialog_palettes_keep_the_hierarchy_without_hue`: high contrast and
  monochrome still separate frame, title, selection and focus (bold/reverse).

Extend `tests/tui_pty_test.cpp`:

- Trigger a real foreground approval at 80×24 and assert the captured screen
  contains `Approve action?`, all four option labels, `auto-deny`, and the
  footer keys. This is the end-to-end proof that the sheet fits a real pty.

## 7. Notices

Background completion, plugin notifications and command results remain
`P_STATUS` transcript lines: they are bounded, scroll with the conversation and
do not steal focus. The modal host deliberately does not own a second
notification channel.

## 8. Non-goals

- No animation, no new palette kinds, no transcript restyle.
- No per-button bevel or individual button shadows; the drop shadow belongs to
  the sheet.
- No mouse interaction changes.
- No redesign of the key map; the state machines stay as they are.

## 9. Cross-references

- `themes.md` — the role table, palette fallbacks and the shadow glyph.
- `render-foundation.md` — `Surface`, `Cell`, `Role`, the terminal adapter.
- `docs/style/tui-design-guide.md` — the long-form design language; its legacy
  `Panel`/pair examples are historical and the modal sections defer here.

# Spec: full-window screens

- **Status:** 0.8. First slice of the screen migration (`#517`).
- **Related:** `render-foundation.md`, `dialogs.md`, `architecture.md`.

## 1. One painter, one host

A screen is a full-window list, not a dialog: it owns the whole terminal while
open, restores the shell when it closes, and has no result beyond the action the
caller takes afterwards. `ScreenView` is the view-model and `ScreenHost` runs it.

```text
ScreenView { title, header, rows[], footer, filter }
        │
   paint_screen ──► Surface ──► ScreenHost ──► Terminal overlay
```

`paint_screen` draws the frame, title, optional tab header, visible rows,
selection bar, scroll markers, search row and footer. Rows are preformatted
strings; header rows are emphasised and not selectable. The painter is pure and
tested without a terminal at 80×24 and 40×10.

`ScreenHost::run(view, key, poll_ms, tick)` paints, reads, calls the key handler
and repaints. `tick` runs on the idle timeout so a live screen (the Work board)
refreshes without a key. The host owns `ModalScope`, adopts the existing curses
session, and restores `kTickTimeoutMs` on every exit path. It never changes the
active window.

## 2. Screens

| Screen | Rows | Keys | Notes |
|---|---|---|---|
| Sessions | date headers + session rows | Up/Down, Enter load, Del remove, `/` search, Esc | `SessionBrowserCore` stays the pure state machine; the screen only renders it |
| Work board | sessions + tasks | Tab section, Up/Down, Enter/a open, x cancel, `/` filter, n new, Esc | live refresh on the 250 ms tick |
| Plugin panels | panel lines | Tab cycle, Up/Down/PgUp/PgDn scroll, Esc/q close, panel `on_key` first | title/footer from `PanelSpec` and `panel_view_state` |

The settings/provider screens already use the modal host: their pickers and
forms are `menu_select`/`form_edit`, and destructive steps are `ConfirmPanel`.
No screen paints a curses window directly; the domain state machines
(`SessionBrowserCore`, `ListState`, `panel_view_state`) are unchanged.

## 3. Tests

- `tests/screen_view_test.cpp` — frame, title/header, selection, search cursor,
  clipping and scroll markers.
- `tests/tui_pty_test.cpp` — the session browser, the Work board and the plugin
  panel viewer end to end, including returning to the prompt.

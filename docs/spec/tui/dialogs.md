# Spec: Dialogs and the modal stack

- **Status:** 0.8. M4 of the shell plan. Every blocking dialog paints through the
  cell grid (`dialog_view`) and runs through one host (`ModalHost`).
- **Issue:** #516.
- **Related:** `render-foundation.md`, `docs/spec/plugins/developer-guide.md`,
  `docs/spec/tui/architecture.md`.

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
  waits on its promise. That is the existing approval/ask contract; M4 does not
  make it asynchronous.
- **Overlay, not a full screen.** The host blits only the dialog surface at the
  centred origin. On close it restores `stdscr` with one full refresh, so the
  frame underneath returns.
- **Timeout restore.** The host sets the read timeout per iteration (250 ms
  while an approval counts down, blocking otherwise) and restores
  `kTickTimeoutMs` on every exit path, including cancel and timeout.
- **Active window is untouched.** No sheet reads or sets the active window. A
  background tab's approval names that tab (`[title] summary`) and leaves focus
  where the user was; the tab carries `!` while the question is pending.
- **Deferred questions.** `EventRouter` queues approvals, plugin asks and API-key
  requests while `modal_open_` is set and pumps one per tick afterwards. No
  nested curses dialog exists anymore.

## 3. Sheets

| Sheet | Constructor | Keys | Result |
|---|---|---|---|
| Confirm | title, message | Tab/arrows toggle, Enter accepts, Esc cancels | bool |
| Approval | summary, timeout, default | `1`-`4` pick+accept, Tab/arrows move, Enter accepts selection, Esc denies | `agent::Approval` |
| Info | title, rows | Up/Down/PgUp/PgDn/Home/End scroll, Enter/Esc/q close | none |
| List | title, items | Up/Down/PgUp/PgDn, Enter select, `/` filter, Esc cancel | index or -1 |
| Form | title, fields, notice | Tab/arrow field navigation, printable insert, Backspace/Delete, Enter accept, Esc cancel | bool + edited values |
| Custom sheet | a caller-owned `PaintedDialog` painter | `Accept`/`Cancel` from the handler; used by the skills install sheet and facet overlay | caller-defined |

The approval countdown **denies on expiry**. It never auto-confirms: the
fail-safe is `ApprovalModel`'s invariant, not the panel's.

Paste into a form is decoded from the bracketed-paste stream before the key
path, so an API key can be pasted without the leading ESC cancelling the dialog.
Control bytes in a paste are dropped; the field stays single-line.

## 4. Layout

```
┌──────────────────────────────────────┐
│           Approve action?            │
│  [worker] bash: echo hi > file       │
│  [1] Allow once (45s)  [2] Allow ... │
│  1-4: pick   Enter: confirm  Esc ...  │
└──────────────────────────────────────┘
```

The view sizes itself to the title, body, fields and buttons, clamps to the
screen, wraps long body lines and places at most one scroll marker per side.
The selected row/button/field uses the `Selected` role; the footer and labels
use `Muted`; a form notice uses `Error`. The form cursor is returned as a cell
position and shown only while a field has focus.

## 5. Notices

Background completion, plugin notifications and command results remain
`P_STATUS` transcript lines: they are bounded, scroll with the conversation and
do not steal focus. The modal host deliberately does not own a second
notification channel.

## 6. Tests

- `tests/dialog_test.cpp` — state machines and painter: confirm toggle, info
  bounds, form editing/paste/buttons, framed rendering at 80×24 and 120×40,
  selection/filter/notice/cursor placement and small-screen clamping.
- `tests/tui_pty_test.cpp` — the real flows: confirm/cancel on a task, provider
  API-key form (including paste), and a background-window approval that names
  its window, keeps the active window, and whose approved command actually runs.
- `tests/gates/` — no separate gate; the ncurses ratchet shrinks as the old
  dialog files stop including curses.

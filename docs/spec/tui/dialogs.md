## Spec: TUI Dialogs (Modal System)

### Purpose
Display modal dialogs for tool approval, error messages, and choices that
block the agent thread (via `std::promise`/`std::future`) without blocking
the TUI's main event loop. Dialogs are ncurses overlays with a framed border.

### Ownership
- **Source files**: `tui/dialog.cpp` (56 lines — `init_pairs()`, dialog frame helpers), `tui/menu_select.cpp` (`menu_select()`), `tui/form_edit.cpp` (`form_edit()`), `tui/info_dialog.cpp` (`info_dialog()`), `tui/list_panel.cpp` (`list_panel()`)
- **Test files**: No direct tests (ncurses-dependent).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `menu_select`: title + options list. `form_edit`: fields + current values. `info_dialog`: title + body text. `list_panel`: items + select callback. |
| **Output** | `menu_select`: selected option index. `form_edit`: edited values via callback. `info_dialog`: dismissed. `list_panel`: selected item. |
| **Error states** | Esc = cancel (returns -1 or empty). Resize = next draw redraws. |
| **Invariants** | See below. |
| **Thread safety** | Agent thread blocks on `promise.get_future().wait()`. Main thread fulfills promise from event loop. |

### Invariants

1. Only one modal is visible at a time (`modal_open_` flag prevents nesting).
2. Approval events that arrive while a modal is open are deferred to `pending_approvals_` queue.
3. The agent thread always blocks during approval (via `std::promise`/`std::future`).
4. Main thread never blocks during approval — the event loop continues processing input.
5. Esc cancels the modal (returns -1 / empty).
6. All modals draw their own ncurses window and call `doupdate()` immediately (not deferred to `dirty_`).

---

### Scenarios

#### [DI-01] Menu select — basic

- **Given**: Approval dialog for bash tool
- **Input**: `menu_select("Approve?", {"Deny", "Allow once", "Allow for this session"})`
- **Expected**: Framed dialog centered on screen. Three options. Up/Down to select. Enter to confirm. Returns selected index (0, 1, 2).
- **On failure**: Dialog not centered, or options not selectable.

#### [DI-02] Menu select — cancel with Esc

- **Given**: Menu dialog open
- **Input**: Esc pressed
- **Expected**: Returns -1. No selection made.
- **On failure**: First option selected instead of cancel.

#### [DI-03] Approval deferred while modal open

- **Given**: Settings dialog open, agent requests bash approval
- **Input**: `Approval` event arrives
- **Expected**: Event handler checks `modal_open_` → pushes to `pending_approvals_`. After settings dialog closes, `redraw_after_modal()` processes one pending approval at a time.
- **On failure**: Approval dialog appears on top of settings (nested modal corruption).

#### [DI-04] Form edit — provider configuration

- **Given**: User editing provider settings
- **Input**: Edits Server URL, API Key, Model, Context Size
- **Expected**: Each field is editable inline. Tab/Enter moves to next field. Esc cancels. Submit returns values.
- **On failure**: Field values not updated, or crash on empty value.

#### [DI-05] Info dialog — display only

- **Given**: Error message or `/help` display
- **Input**: `info_dialog("Error", "Connection refused")`
- **Expected**: Framed dialog with body text. Any key dismisses. No selection.
- **On failure**: Dialog blocks indefinitely.

#### [DI-06] List panel — model selection

- **Given**: `/model` command fetches model list
- **Input**: `list_panel(items, "Select model")`
- **Expected**: Scrollable list of model names. Up/Down to navigate. Enter to select. Esc to cancel. Returns selected item index.
- **On failure**: List empty or not scrollable.

#### [DI-07] Resize during modal

- **Given**: Modal dialog open, terminal resized
- **Input**: `SIGWINCH` (no handler), next draw()
- **Expected**: Dialog position recalculated from new dimensions on next draw. Content reflows to new size.
- **On failure**: Dialog drawn at old position (off-screen).

#### [DI-08] Multiple pending approvals processed sequentially

- **Given**: Two approval events queued during modal
- **Input**: Modal closed
- **Expected**: `redraw_after_modal()` processes first approval → user selects → dialog closes → second approval processed. Only one approval dialog visible at a time.
- **On failure**: Two approval dialogs visible simultaneously.

---

### Cross-references

- **Depends on**: `tui/layout-engine.md` (dialog positioning), `tui/event-loop.md` (approval event flow)
- **Depended on by**: `tui/input-system/slash-engine.md` (settings dialog), `tui/settings-ui.md`
- **Test coverage**: No direct tests.

### Known gaps

1. **No modal stacking** — Only one modal at a time. Deferred approvals queue is sequential.
2. **Dialog positioning is always centered** — No option for side panels or bottom sheets.
3. **No keyboard shortcut hints** — Each dialog must self-document its key bindings (OK/Cancel, Up/Down selection).

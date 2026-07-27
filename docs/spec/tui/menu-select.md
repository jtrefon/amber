## Spec: TUI Menu Select

### Purpose
A reusable ncurses selection widget used by approval dialogs, provider
selection, model lists, and the multi-tab completion popup. Renders a
scrollable, framed list with keyboard navigation.

### Ownership
- **Source files**: `tui/menu_select.cpp` (`menu_select()` function), `tui/widgets.h` (declaration)
- **Test files**: No direct tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Title string + vector of option strings + optional width/height constraints |
| **Output** | Selected index (0..N-1), or -1 if cancelled |
| **Error states** | Empty options → returns -1. Terminal too small → may clip but not crash. |
| **Invariants** | See below. |
| **Thread safety** | Runs on main thread only (called from event loop). |

### Invariants

1. Up/Down arrows navigate the selection list.
2. Enter confirms the current selection.
3. Esc cancels and returns -1.
4. The list is scrolled (not wrapped) if options exceed visible rows.
5. The selected item is highlighted with `A_REVERSE`.
6. The dialog has a framed border with the title centred at the top.
7. The dialog is centered on the screen at its current size.

---

### Scenarios

#### [MS-01] Three options — selection

- **Given**: `menu_select("Approve?", {"Deny", "Allow once", "Allow all"})`
- **Input**: Down arrow → Enter
- **Expected**: Selection starts at 0 (Deny). Down moves to 1. Enter returns 1.
- **On failure**: Wrong index returned.

#### [MS-02] Cancel with Esc

- **Given**: Any menu
- **Input**: Esc
- **Expected**: Returns -1. No side effect.

#### [MS-03] Many options — scroll

- **Given**: 20 options, terminal shows 10
- **Input**: Down arrow repeatedly past visible area
- **Expected**: List scrolls. Selection moves through all 20 options. Back up from top scrolls in reverse.
- **On failure**: Selection stuck at visible boundary.

#### [MS-04] Popup completion — multi-tab

- **Given**: `Completer` triggers popup with completion candidates
- **Input**: Two consecutive Tabs after ambiguous prefix
- **Expected**: `menu_select()` pops up with candidates. User selects → input line completed.
- **On failure**: Popup doesn't show or wrong completion applied.

#### [MS-05] Empty options

- **Given**: No options to show
- **Input**: `menu_select("Empty", {})`
- **Expected**: Returns -1 immediately.
- **On failure**: Crash or empty dialog displayed.

---

### Cross-references

- **Depends on**: `tui/layout-engine.md` (dialog positioning), `tui/dialogs.md`
- **Depended on by**: `tui/input-system/auto-complete.md` (popup), `tui/dialogs.md` (approval), `tui/settings-ui.md`
- **Test coverage**: No direct tests.

### Known gaps

1. **No search/filter within menu** — Users must scroll through all options.
2. **No multi-select** — Only single selection supported.
3. **No typeahead** — Typing characters does not jump to matching option.

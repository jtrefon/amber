# amber TUI Design Guide

- **Status:** Living standard. The modal layer (§2.2–§2.4, §3.1, §3.4,
  §4.3–§4.5 and §9) is specified by `docs/spec/tui/dialogs.md`; where this guide
  shows the legacy ncurses `Panel`/pair API, that example is historical.
- **Applies to:** `tui/` (ncurses client)
- **Inspiration:** Midnight Commander, Vim, htop, Norton Commander, dialog, CDK
- **Last updated:** 2026-11-15

---

## 1. Core Principles

### 1.1 Keyboard-first

The TUI is keyboard-driven. Every action must be reachable without a mouse.
Mouse support is optional and must never be the only way to perform an action.

### 1.2 Consistent navigation

| Key | Action |
|-----|--------|
| `Up`/`Down` or `j`/`k` | Navigate lists |
| `PgUp`/`PgDn` | Scroll by page |
| `Enter` | Confirm / select / activate |
| `Esc` | Cancel / close / back |
| `Tab` | Next focusable element |
| `Shift-Tab` | Previous focusable element |
| `F1` | Help |
| `Ctrl-D` or `Del` | Delete selected item |

### 1.3 Visual hierarchy

Every screen follows a three-zone layout:

```
┌─────────────────────────────────────────┐
│  Title bar                               │  ← context (what are we looking at)
├─────────────────────────────────────────┤
│                                          │
│  Content area                            │  ← the work (list, text, form, chat)
│                                          │
├─────────────────────────────────────────┤
│  [F1 Help] [Enter OK] [Esc Cancel]      │  ← footer (available actions)
└─────────────────────────────────────────┘
```

---

## 2. Window and Panel Styling

### 2.1 Border style

Use ACS line drawing characters for all borders. Never use ASCII characters
like `+`, `-`, `|` for framing.

```
┌─── Correct ───┐    +-- Wrong --+
│                │    |           |
└────────────────┘    +-----------+
```

- **Active/focused window**: solid border with title
- **Modal dialog**: solid border with drop shadow (1 row down, 1 col right)
- **Status bar**: no border, full-width, inverse/colored background

### 2.2 Title bar

Titles are placed in the top border, centered, between `┤` and `├`:

```
┌───┤ Sessions ├────────────────────────────┐
│                                           │
└───────────────────────────────────────────┘
```

Implementation:
```cpp
int t = (width - title.size()) / 2;
if (t < 2) t = 2;
mvwaddch(win, 0, t - 1, ACS_RTEE);
mvwprintw(win, 0, t, "%s", title.c_str());
mvwaddch(win, 0, t + title.size(), ACS_LTEE);
```

The title segment sits on the sheet background; painting the terminal default
behind it punches a hole in the frame (see `docs/spec/tui/dialogs.md` §4.4).

### 2.3 Footer bar

Action shortcuts in the bottom border, with the key as a filled chip:

```
└─ [1-4] Pick  [Tab] Move  [Enter] Confirm  [Esc] Cancel ──────┘
```

Keys are bold on a contrasting background; descriptions sit on the sheet
background. The footer is the bottom border row, not an extra body row
(`docs/spec/tui/dialogs.md` §4.5).

### 2.4 Drop shadow

Modals cast a shadow 1 row below and 1 col to the right. The shadow must remain
visible on a black terminal, so it is a shaded glyph in the `Shadow` role, not
a solid black block:

```
┌──────────────┐
│ Dialog       │░
│              │░
└──────────────┘░
 ░░░░░░░░░░░░░░░
```

---

## 3. Color Palette

### 3.1 Standard pairs

| ID | Foreground | Background | Usage |
|----|-----------|-----------|-------|
| `P_USER` | Green | Default | User text in chat |
| `P_ASSISTANT` | Cyan | Default | Assistant replies |
| `P_STATUS` | Yellow | Default | Tool output, status messages |
| `P_DEBUG` | Magenta | Default | Debug traces |
| `P_REASONING` | White (dim) | Default | Model thinking/reasoning |
| `P_BANNER` | White | Blue | Status bar background |
| `P_DIALOG` | White | Blue | Dialog sheet background (the modal roles are in `themes.md`) |
| `P_BUTTON` | Black | Cyan | Unselected button in the legacy pair layer |
| `P_BUTTON_ACT` | Black | White | Focused button / selected row |
| `P_SHADOW` | Dark gray | Default | Drop shadow, drawn with `░` (not a solid block) |
| `P_FIELD` | Black | White | Form field background |
| `P_FIELD_ACT` | White | Cyan | Active form field |

### 3.2 Status bar gauge

| ID | Foreground | Background | Meaning |
|----|-----------|-----------|---------|
| `P_GAUGE_OK` | Green | Blue | Context < 60% |
| `P_GAUGE_WARN` | Yellow | Blue | Context 60-85% |
| `P_GAUGE_CRIT` | Red | Blue | Context > 85% |
| `P_BAR_DIM` | Cyan | Blue | Faint labels, gauge track |

### 3.3 Markdown / syntax highlighting

| ID | Usage |
|----|-------|
| `P_MD_HEAD` | Headings |
| `P_MD_QUOTE` | Block quotes |
| `P_MD_CODE` | Inline code, fenced code blocks |
| `P_MD_CODEKEY` | Syntax highlight: keyword |
| `P_MD_CODESTR` | Syntax highlight: string |
| `P_MD_CODENUM` | Syntax highlight: number |
| `P_MD_CODECMT` | Syntax highlight: comment |
| `P_MD_LINK` | Link text |
| `P_MD_TABLE` | Table rows |
| `P_MD_HR` | Horizontal rule |

### 3.4 Selection highlight

A selection is a full-width bar in `DialogSelected` (black on cyan in
Amber/Modern), not a text-only attribute change. Every cell from the inner left
edge to the inner right edge of the row is painted, so the eye reads one bar:

```cpp
surface.fill({1, row, width - 2, 1}, Cell{" ", Style{Role::DialogSelected}});
surface.put_text(2, row, item, Style{Role::DialogSelected});
```

The legacy pair layer's `A_REVERSE` examples remain valid only for screens
that have not migrated to the cell grid; a modal uses the dialog roles.

---

## 4. Widget Patterns

### 4.1 Panel (base framed window)

```
┌─── Title ────────────────────────────────┐
│                                           │
│  Content area (derwin)                    │
│                                           │
│  [Up/Down] nav  [Enter] ok  [Esc] cancel │
└───────────────────────────────────────────┘
```

All modal components inherit from `Panel`:
- `ListPanel` — scrollable selection list
- `ConfirmPanel` — Yes/No confirmation
- `InfoPanel` — scrollable read-only text
- `FormPanel` — editable fields
- `ConfigPanel` — settings editor

### 4.2 List panel

```
┌─── Sessions ──────────────────────────────┐
│  > Today's session             3 msgs     │  ← selected (reverse highlight)
│    Yesterday's chat            12 msgs    │
│  ▴                                        │  ← scroll up indicator
│    Old conversation           45 msgs     │
│  ▾                                        │  ← scroll down indicator
│                                           │
│  / search_filter                         │  ← filter bar
│  [Up/Down] nav  [Enter] load  [Del] del  │
└───────────────────────────────────────────┘
```

Scroll indicators use `ACS_UARROW` / `ACS_DARROW` at the edge of the content area.

Filter bar at the bottom (before footer) with `/ ` prefix.

### 4.3 Confirmation dialog

```
┌──┤ Delete Session ├──────────────────────────────┐
│ Delete "Today's work"?                           │
│                                                  │
│            [ Yes ]       [ No ]                  │
└─ [Tab] Switch  [Enter] Confirm  [Esc] Cancel ────┘
```

- Default to "No" (safe default), drawn as a filled block in
  `DialogButtonFocus`; "Yes" is `DialogButton` (black on cyan).
- Tab/Left/Right switch, Enter confirms, Esc cancels (equivalent to No).
- The footer is the bottom border row; the buttons are one row above it.

### 4.4 Info dialog

```
┌─── Model Info ────────────────────────────┐
│  Model: qwen                              │
│  Context: 32768                           │
│  Provider: llama.cpp                      │
│                                           │
│  ▴                                        │  ← scroll if content overflows
│  more content...                          │
│  ▾                                        │
│                                           │
│  [Up/Down] scroll  [Enter/Esc] close      │
└───────────────────────────────────────────┘
```

### 4.5 Form / settings panel

```
┌─── Settings ──────────────────────────────┐
│                                           │
│  API Base: ┌─────────────────────┐        │
│            │ http://localhost... │        │  ← active field (cyan bg)
│            └─────────────────────┘        │
│  API Key:  ┌─────────────────────┐        │
│            │ ******************* │        │  ← secret field
│            └─────────────────────┘        │
│  Model:    ┌─────────────────────┐        │
│            │ qwen                │        │
│            └─────────────────────┘        │
│                                           │
│  [Tab] next  [Enter] edit  [Esc] back    │
└───────────────────────────────────────────┘
```

Use libform (`-lformw`) for actual form fields. Active field has cyan background,
inactive fields have white background.

---

## 5. Layout

### 5.1 Main window layout

```
┌──────────────────────────────────────────┐
│  Chat area (scrollback)                  │  ← 0 to status-2
│                                          │
│                                          │
│  user@host:~$ _                          │  ← input line (row status-1)
│  [model] mode  ctx ▓▓░░ 0%  [clock]     │  ← status bar (row status)
└──────────────────────────────────────────┘
```

### 5.2 Spacing

- Content inside borders: 1 cell padding on all sides
- Between label and value (forms): 1 space
- Between buttons: 2 spaces
- Between list items: 0 (dense lists, no blank lines between items)
- Filter bar in list: 1 row above footer

### 5.3 Sizing

- Modal dialogs: centered, min 40 cols, max 80% of terminal width
- List panels: height = min(items + 2, 80% of terminal height)
- Confirm dialogs: 7 rows, width = max(message + 6, 40)
- Info dialogs: 60% of terminal dimensions
- Session browser: 80% of terminal, min 80 cols

---

## 6. Keyboard Conventions

### 6.1 Universal

| Key | Action | Context |
|-----|--------|---------|
| `Esc` | Cancel / close / go back | All contexts |
| `q` | Close / quit current view | Dialogs, lists |
| `Enter` | Confirm / select / activate | All contexts |
| `Tab` | Next focusable element | Forms, dialogs |
| `Shift-Tab` | Previous focusable element | Forms, dialogs |

### 6.2 List navigation

| Key | Action |
|-----|--------|
| `Up` / `k` | Move selection up |
| `Down` / `j` | Move selection down |
| `PgUp` | Scroll up one page |
| `PgDn` | Scroll down one page |
| `Home` / `g` | Jump to first item |
| `End` / `G` | Jump to last item |

### 6.3 Text input

| Key | Action |
|-----|--------|
| Printable chars | Insert into input buffer |
| `Backspace` / `Del` | Delete character before cursor |
| `Ctrl-U` | Clear input buffer |
| `Enter` | Submit input |
| `Esc` | Cancel input / close drawer |
| `Tab` | Complete from history / trigger completion drawer |

### 6.4 Chat view

| Key | Action |
|-----|--------|
| `Up` / `Down` | Scroll chat history |
| `PgUp` / `PgDn` | Page through history |
| `End` | Jump to newest messages |
| `Enter` (on empty input) | Repeat last prompt |

### 6.5 Session browser

| Key | Action |
|-----|--------|
| `Up` / `Down` | Navigate sessions |
| `Enter` | Load selected session |
| `Del` / `Ctrl-D` | Delete selected session (with confirmation) |
| `/` | Start filter mode |
| `Esc` | Clear filter / close browser |
| `q` | Close browser |

---

## 7. Status Bar

```
[qwen] write  lag 0.3s  — t/s  ████████░░ 80%  128k/160k  [19:22:18]
```

Left to right:
1. Model name in brackets (cyan bold)
2. Mode: `read`, `write`, `yolo` (normal)
3. Connection: `lag Ns` (yellow if > 1s, red if > 5s)
4. Token rate: `N t/s` (if streaming)
5. Context gauge: `▓▓▓▓░░` with percentage and token count
6. Clock: `[HH:MM:SS]` (dim)

During tool execution:
```
[qwen] write  bash: make -j4  lag —  t/s  ctx ▓▓▓▓░░ 80%  128k/160k  [19:22:18]
```

Running tool name inserted after mode, replacing connection indicator.

---

## 8. Error States

### 8.1 Error display

Errors in chat appear as:
```
─── tool:read ──────────────────────────────
  path: /nonexistent
  status: error
────────────────────────────────────────────
ERROR: path escapes workspace root
─── end ────────────────────────────────────
```

In dialogs, errors appear as an `InfoPanel` with red-tinted title.

### 8.2 Permission denied

When a tool is denied (e.g. bash without approval):
```
─── tool:bash ──────────────────────────────
  command: rm -rf /
  status: denied
────────────────────────────────────────────
ERROR: denied by user: bash was not approved
─── end ────────────────────────────────────
```

---

## 9. Implementation Checklist

When creating a new sheet or screen:

- [ ] Uses `ModalHost`/`DialogView` for a blocking dialog, not a raw ncurses
      `WINDOW`/`PANEL`
- [ ] Uses `Surface`/`Cell` and semantic `Role`s, not hardcoded colour pairs
- [ ] Has a title in the top border, surrounded by `┤`/`├`
- [ ] Has a footer key bar in the bottom border (not a body row)
- [ ] Uses `DialogSurface`/`DialogText`/`DialogMuted` inside a sheet; no
      `Text`/`Muted` cells that paint the terminal default behind the sheet
- [ ] Has a visible one-cell drop shadow using the `Shadow` role and `░`
- [ ] Draws buttons as filled blocks, inside the frame, never clipped
- [ ] Draws a selected row as a full-width `DialogSelected` bar
- [ ] Uses `Glyphs` for borders (`+ - |` in ASCII), never hardcoded UTF-8
- [ ] Supports `Esc` to cancel/close
- [ ] Supports Tab/arrow navigation for multiple focusable elements
- [ ] Shows scroll markers when content overflows
- [ ] Uses `ModalScope` if blocking (sets the modal flag)
- [ ] Fits 80×24 and 120×40; paints nothing outside the screen

---

## 10. Reference: Professional TUI Applications

### Midnight Commander (MC)
- Dual-panel file browser, orthodox commander style
- Function keys F1-F10 for all major actions
- Drop-down menu bar at top
- Command line below panels
- Consistent: every screen has title, content, and function key bar

### htop
- Process listing with scrollable list
- Color-coded meters (CPU, memory)
- Footer bar with function key actions
- Column-based sortable display
- Consistent: setup screen → list → detail, all using same color scheme

### Vim / Neovim
- Modal editing with clear mode indicators
- Status line with file info, cursor position, and mode
- Command line for text input
- Consistent: `Esc` always goes back to Normal mode

### dialog (terminal dialog boxes)
- Calendar, checklist, form, gauge, inputbox, menu, msgbox, passwordbox,
  radiolist, tailbox, textbox, timebox, treeview
- All use the same border style, same color scheme, same keybindings
- Title in top border, buttons in bottom
- Consistent sizing and positioning

### Common patterns across all

| Pattern | MC | htop | Vim | dialog | Our approach |
|---------|----|------|-----|--------|-------------|
| Footer keys | F1-F10 bar | F1-F10 bar | Status line | Bottom buttons | ACS-bordered footer |
| Scroll indicator | Arrow | Scrollbar | Scrollbar | — | ACS_UARROW/DARROW |
| Color theme | Customizable | Fixed | Customizable | Customizable | Pair enum |
| Modal dialogs | Yes | No | No | Yes | Panel + shadow |
| List nav | Up/Down/PgUp/PgDn | Up/Down | j/k | Up/Down | Up/Down + j/k |

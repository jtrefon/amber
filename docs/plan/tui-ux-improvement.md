# TUI UX Improvement Plan

- **Status:** Draft (preparation phase)
- **PR target:** Single PR, multiple commits, no architecture changes
- **Prerequisite research:**
  - `docs/terminal-compatibility.md` — cross-terminal rendering
  - `docs/style/tui-design-guide.md` — widget style bible
  - `docs/ncurses.md` — ncurses API reference
  - `docs/issues.md` — original audit (all resolved)
  - MC source: `lib/tty/tty-ncurses.c`, `lib/skin/lines.c`
  - htop source: `CRT.c`

---

## 1. What's Broken

### 1.1 Summary of 33 audit violations

From the style guide audit (see PR #13), 33 violations were found. 7 were
fixed in that PR. 26 remain.

| Area | Remaining violations | Impact |
|---|---|---|
| **Borders** | `form_edit`, `info_dialog`, `tui_session` use old `Dialog` class | Inconsistent with `Panel`-based components. Some use Unicode literals, others ACS. |
| **Footers** | `form_edit`, `info_dialog`, `tui_session` place footers inside content area | Inconsistent with `Panel`'s bottom-border footer. Missing bold-key styling. |
| **Selection** | `tui_session` browser, drawer menu use `P_BUTTON_ACT` + `"> "` instead of `A_REVERSE` | Three different selection styles in the same app. |
| **Scroll indicators** | `info_dialog`, `tui_session` browser missing ACS_UARROW/ACS_DARROW | User can't tell if content extends beyond viewport. |
| **Colour** | `P_FIELD_ACT` previously wrong (FIXED), but `P_BUTTON_ACT` uses `COLOR_YELLOW` bg which may not work on all terminals | Style guide says `COLOR_WHITE, COLOR_BLACK` for selected button (reverse). |
| **Missing filter bar** | `ListPanel` lacks search/filter input | Session browser has one, but `ListPanel` (used by `menu_select`) doesn't. |
| **Inconsistent widget hierarchy** | `Dialog` vs `Panel` — two different implementations for the same thing | All modals should look and behave identically. |
| **Status bar** | New `StatusBar` skeleton is empty | No model/mode/lag/tps/gauge/clock. |

### 1.2 Terminal compatibility issues

| Issue | Affected environments | Root cause |
|---|---|---|
| q/x/m/j instead of lines | Old PuTTY, misconfigured locale | Unicode literals in `Dialog` class bypass ncurses' ACS handling. Fixed by `box()` in PR #13. |
| Box-drawing fails on `TERM=linux` | Linux console (Ctrl+Alt+F2) | Same as above. |
| `P_BUTTON_ACT` uses `COLOR_YELLOW` bg | Terminals with low colour contrast | Yellow on white may be invisible. Should use `A_REVERSE` like all other selection. |
| `P_FIELD_ACT` was `COLOR_BLACK` bg (FIXED) | All terminals with dark bg | Active field looked identical to inactive field. Fixed in PR #13. |
| No ASCII fallback | Linux console, serial terminals, accessibility | When neither ACS nor Unicode works, there's no `+`/`-`/`|` fallback. |

### 1.3 Usability gaps

| Gap | Where | What's missing |
|---|---|---|
| Delete confirmation | Session browser only | No way to delete a session from outside the browser. No "Are you sure?" for other destructive actions. |
| Filter/Search | `menu_select` (ListPanel) | Session browser has `/` filter, but `ListPanel` used by `menu_select` doesn't. |
| Keyboard consistency | Drawer menu, session browser | Different keys for same actions across components. |
| Visual feedback | Status bar | No running tool indicator, no lag colour changes, no clock. |
| Help | Everywhere | No `F1` handler. No help popup in any dialog. |

---

## 2. How Others Solved These Problems

### 2.1 Midnight Commander — Border rendering

MC's three-tier approach (from `lib/skin/lines.c` and `tty-ncurses.c`):

```
UTF-8 locale? → use setcchar() with raw Unicode codepoints, no ACS at all
8-bit locale? → convert to MC_ACS_* constants → ncurses addch()
                If double-line → try iconv to locale, fallback to single
                If --ugly flag → ASCII + - |
                If --slow flag → spaces (minimises output)
```

**What we can use**: The two-tier (Unicode → ASCII) is sufficient for amber.
htop proves this works everywhere without the complexity of MC's iconv
double-line handling. We do not need double-line borders.

### 2.2 htop — Tree rendering (cross-terminal)

htop maintains two hardcoded string tables and selects based on `nl_langinfo(CODESET)`:

```c
static const char* const CRT_treeStrAscii[LAST_TREE_STR] = {
    [TREE_STR_VERT] = "|",   [TREE_STR_RTEE] = "`",
    [TREE_STR_BEND] = "`",   [TREE_STR_TEND] = ",",
    [TREE_STR_OPEN] = "+",   [TREE_STR_SHUT] = "-",
};
static const char* const CRT_treeStrUtf8[LAST_TREE_STR] = {
    [TREE_STR_VERT] = "\xe2\x94\x82",  // │
    [TREE_STR_RTEE] = "\xe2\x94\x9c",  // ├
    // ...
};
CRT_treeStr = CRT_utf8 ? CRT_treeStrUtf8 : CRT_treeStrAscii;
```

**What we can use**: Same pattern for our border/tree/indicator characters.
Hardcoded UTF-8 bytes are more portable than `ACS_*` because they don't
depend on terminfo's `acsc`. htop's approach: if UTF-8 is active, use
Unicode bytes; otherwise use ASCII. No ACS at all.

### 2.3 Vim — Per-terminal configuration

Vim uses `t_*` options that users can override per terminal:
```
:set t_Co=256       # force 256 colours
:set t_vb=          # disable visual bell
```

**What we can use**: We don't need user-configurable escape sequences,
but we should respect `$TERM` and `$COLORTERM` for colour detection.

### 2.4 ncurses best practices (synthesized)

From the ncurses source and the NCURSES Programming HOWTO:

1. **Always call `setlocale(LC_ALL, "")` first** — determines Unicode vs ACS path
2. **Use `box(win, 0, 0)` for borders** — canonical, ncurses handles the rest
3. **Use `A_REVERSE` for selection** — most portable highlight attribute
4. **Use `COLOR_PAIR(n)` for colours** — never hardcode colour numbers
5. **Check `has_colors()` before `start_color()`** — some terminals lack colour
6. **Use `use_default_colors()`** — allows `-1` for terminal default bg
7. **Document `NCURSES_NO_UTF8_ACS=1`** — escape hatch for PuTTY users

---

## 3. Architecture Decisions

### 3.1 One widget hierarchy: `Panel` only

**Decision**: The `Dialog` class will be refactored to inherit from `Panel`,
eliminating the two inconsistent widget hierarchies.

Current state:
```
Dialog (standalone, used by form_edit, info_dialog, session_browser)
  └── form_edit.cpp     ← inherits Dialog
  └── info_dialog.cpp   ← inherits Dialog
  └── tui_session.cpp   ← uses Dialog directly

Panel (base, used by ListPanel, ConfirmPanel)
  └── ListPanel         ← inherits Panel
  └── ConfirmPanel      ← inherits Panel
```

Target state:
```
Panel  ← the only base class for framed windows
  ├── ListPanel         ← existing, kept as-is
  ├── ConfirmPanel      ← existing, kept as-is
  ├── InfoPanel         ← new (replaces info_dialog)
  ├── FormPanel         ← new (replaces form_edit internals)
  └── Dialog            ← refactored to inherit Panel
       └── SessionBrowser  ← new (extracted from tui_session.cpp)
```

### 3.2 Border rendering: two-tier (Unicode → ASCII)

**Decision**: Replace ACS with htop's two-tier approach for all custom
rendering. Use `box()` for simple windows, and a `BorderRenderer` helper
for cases needing custom characters.

```cpp
// BorderRenderer selects glyphs based on UTF-8 capability
struct GlyphSet {
    const char* hline = "-";
    const char* vline = "|";
    const char* ul    = "+";
    const char* ur    = "+";
    const char* ll    = "+";
    const char* lr    = "+";
    const char* up    = "^";
    const char* dn    = "v";
};

GlyphSet detect_glyphs() {
    if (utf8_locale()) {
        return {
            "\xe2\x94\x80",  // ─
            "\xe2\x94\x82",  // │
            "\xe2\x94\x8c",  // ┌
            "\xe2\x94\x90",  // ┐
            "\xe2\x94\x94",  // └
            "\xe2\x94\x98",  // ┘
            "\xe2\x86\x91",  // ↑
            "\xe2\x86\x93",  // ↓
        };
    }
    return {};  // ASCII defaults
}
```

### 3.3 Selection: `A_REVERSE` everywhere

**Decision**: All list/item selection uses `A_REVERSE`. No custom colours,
no `"> "` prefix, no fill bars. This is the ncurses convention used by
MC, htop, and dialog.

### 3.4 Footer: bottom-border styled keys

**Decision**: All dialogs have a footer in the bottom border with keys in
bold and descriptions in normal weight. The `Panel` base class provides
this. The `Dialog` class (after refactoring) will get it too.

### 3.5 Status bar: complete implementation

**Decision**: The new `StatusBar` component will render the full format:
```
[model] mode [tool] lag Ns t/s gauge [clock]
```

With colour changes for lag thresholds (>1s yellow, >5s red).

### 3.6 Keyboard: universal convention

**Decision**: All components follow the universal key table from the
design guide (Sec 6.1). Any component that doesn't support Esc/Enter/Tab
will be fixed.

---

## 4. Implementation Plan

### Phase 1: Fix old Dialog-based components (form_edit, info_dialog, session_browser)

| Task | Files | Approach |
|---|---|---|
| 1.1 Refactor `Dialog` to inherit `Panel` | `dialog.h`, `dialog.cpp`, `panel.h` | `Dialog` becomes a thin wrapper around `Panel` with same API |
| 1.2 Add footer support to `Dialog` | `dialog.cpp` | Leverage `Panel`'s footer mechanism |
| 1.3 Fix `form_edit` footer styling | `form_edit.cpp` | Use bold-key format in Panel's footer |
| 1.4 Fix `info_dialog` scroll + footer | `info_dialog.cpp` | Add scroll indicators, styled footer |
| 1.5 Fix session browser selection + scroll | `tui_session.cpp` | Replace `P_BUTTON_ACT` fill bar with `A_REVERSE`. Add scroll indicator |
| 1.6 Fix session browser footer | `tui_session.cpp` | Use styled bold-key footer in bottom border |

### Phase 2: Add missing features

| Task | Files | Approach |
|---|---|---|
| 2.1 Add filter bar to `ListPanel` | `list_panel.h`, `list_panel.cpp` | `/ ` prefix filter input at bottom of list |
| 2.2 Add delete action to window manager | `tui.cpp`, `tui_input.cpp` | `/session delete` command |
| 2.3 Add `F1` help to all dialogs | `Panel` base class | Universal help popup handler |
| 2.4 Fix `P_BUTTON_ACT` colour | `dialog.cpp` init_pairs | `COLOR_BLACK, COLOR_WHITE` (reverse) |

### Phase 3: Status bar + border consistency

| Task | Files | Approach |
|---|---|---|
| 3.1 Implement full `StatusBar` | `status_bar.cpp` | Model, mode, tool, lag, t/s, gauge, clock |
| 3.2 Add border glyph detection | `GlyphSet` in `panel.cpp` | UTF-8 → Unicode, else → ASCII |
| 3.3 Ensure `box()` everywhere | `dialog.cpp`, `panel.cpp` | Already done for Panel, verify Dialog uses box() |

### Phase 4: Final cross-terminal verification

| Task | Approach |
|---|---|
| 4.1 Test on `TERM=linux` | Linux console via Ctrl+Alt+F2 |
| 4.2 Test on `xterm-256color` | Default xterm |
| 4.3 Test on `tmux` / `screen` | Inside tmux session |
| 4.4 Test over SSH | SSH to localhost or remote |
| 4.5 Test with `NCURSES_NO_UTF8_ACS=1` | Force ACS mode |
| 4.6 Test on macOS Terminal | If available |

---

## 5. Verification

Each phase must pass:
- `make clean && make && make test` — 143 tests, zero failures
- `make lint` — zero new clang-tidy warnings
- `make analyze` — zero new cppcheck warnings
- Visual verification on at least 3 different terminal types

---

## 6. Files to change

| File | Phase | Change |
|---|---|---|
| `tui/panel.h` | 1.1 | Add `set_footer()`, `set_title()` methods. Add `F1` help. |
| `tui/panel.cpp` | 1.1 | Implement `set_footer()`, `set_title()`. Add help popup. |
| `tui/dialog.h` | 1.1 | Change to inherit `Panel`. Keep same public API. Header becomes thin wrapper. |
| `tui/dialog.cpp` | 1.1 | Delegate to `Panel`. Keep `init_pairs()`. Remove duplicate rendering. |
| `tui/form_edit.cpp` | 1.2 | Use Panel's footer. Fix styling. |
| `tui/info_dialog.cpp` | 1.4 | Use Panel's footer. Add scroll indicators. |
| `tui/tui_session.cpp` | 1.5, 1.6 | Replace selection style. Add scroll indicators. Fix footer. |
| `tui/list_panel.h` | 2.1 | Add filter state. |
| `tui/list_panel.cpp` | 2.1 | Implement filter input. |
| `tui/confirm_panel.h` | — | Already compliant (default No fixed). |
| `tui/confirm_panel.cpp` | — | Already compliant. |
| `tui/status_bar.h` | 3.1 | Add model/mode/tool/lag/tps/gauge/clock fields. |
| `tui/status_bar.cpp` | 3.1 | Implement full status bar render. |
| `tui/tui.cpp` | 2.2, 3.1 | Add /session delete. Wire new StatusBar. |
| `tui/tui_input.cpp` | 2.2 | Add session delete command. |
| `tui/renderer.cpp` | 3.1 | Draw status bar using new format. |
| `tui/widgets.h` | 1.1 | Dialog declaration stays same (API compat). |

---

## 7. Summary

This plan addresses:

| Problem | How |
|---|---|
| 26 remaining style violations | Phases 1-3 fix all of them |
| Terminal compatibility (q/x/m/j) | Already fixed in PR #13 (box() + setlocale) |
| Unicode rendering on Linux console | `box()` delegates to ncurses which uses ACS for TERM=linux |
| No ASCII fallback | Phase 3.2 adds GlyphSet detection |
| No filter in ListPanel | Phase 2.1 adds it |
| No delete session action | Phase 2.2 adds it |
| No F1 help | Phase 2.3 adds it |
| Empty StatusBar | Phase 3.1 fills it |
| Inconsistent selection | Phase 1.5, 1.6 unify on A_REVERSE |
| Inconsistent footer | Phase 1.2-1.4 unify on Panel's footer |

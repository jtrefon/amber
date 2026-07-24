# Terminal Compatibility Guide

- **Status:** Living reference
- **Applies to:** `tui/` (ncurses client)
- **References:** Midnight Commander, htop, Vim, ncurses source
- **Last updated:** 2026-07-24

---

## 1. Why Terminals Behave Differently

Every terminal emulator advertises its capabilities via the `TERM` environment
variable, which selects an entry in the `terminfo` database. This entry defines
everything: what escape sequences work, what colours are available, what line-
drawing characters can be used, and what keys are supported.

When a character renders as `q`/`x`/`m`/`j` instead of a line:

```
  ┌── intended ──┐       lk ── what you see ── q
  │              │       x                    x
  └──────────────┘       mj
```

This is the ACS (Alternate Character Set) "fallback pathology": ncurses
outputs the one-letter index character for the ACS glyph, but the terminal
does not have that glyph in its current character set mapping. The index
chars are:

| Index | ACS constant | Intended glyph |
|-------|-------------|----------------|
| `q` | `ACS_HLINE` | ─ |
| `x` | `ACS_VLINE` | │ |
| `l` | `ACS_ULCORNER` | ┌ |
| `k` | `ACS_URCORNER` | ┐ |
| `m` | `ACS_LLCORNER` | └ |
| `j` | `ACS_LRCORNER` | ┘ |

---

## 2. How ncurses Actually Works

### 2.1 The two rendering paths

ncurses has two code paths for line drawing:

| Path | Mechanism | When used | Output |
|---|---|---|---|
| **ACS** | Sends VT100 escape `\033(0` to switch to DEC Special Character Set, then sends single-byte index chars (`q`/`x`/`m`/`j`) | `setlocale()` NOT called, or terminal lacks UTF-8, or `NCURSES_NO_UTF8_ACS=1` | Short, portable (no multibyte). Works on all terminals with proper `acsc` terminfo |
| **Unicode** | Outputs multibyte UTF-8 codepoints (`U+2500`–`U+257F`) directly | ncursesw + `setlocale(LC_ALL, "")` + UTF-8 locale + terminal supports UTF-8 | Long, clean. Should work on all modern terminals |

### 2.2 The selection logic

```
                     ┌───────────────────────────┐
                     │  setlocale(LC_ALL, "")?    │
                     └──────────┬────────────────┘
                                │
                   ┌────────────┴────────────┐
                   ▼                         ▼
          UTF-8 locale              non-UTF-8 locale
                   │                         │
                   ▼                         ▼
      ┌──────────────────────┐      ┌──────────────────┐
      │ ncursesw + LC_ALL="" │      │ ACS path via     │
      │ → outputs UTF-8      │      │ addch(ACS_*)     │
      │ box-drawing codepoints│     │                  │
      │                      │      │ Terminfo's acsc  │
      │ Falls back to ACS if │      │ maps index→glyph │
      │ terminal can't handle│      │                  │
      │ UTF-8 (e.g. linux)   │      │ On vt100/linux   │
      └──────────────────────┘      │ this works; on   │
                                    │ misconfigured    │
                                    │ systems → q/x/m/j│
                                    └──────────────────┘
```

### 2.3 The critical `setlocale(LC_ALL, "")` call

This one call determines the entire rendering path. It must be:

```cpp
#include <locale.h>

int main() {
    setlocale(LC_ALL, "");  // MUST be before any ncurses call
    initscr();
    // ...
}
```

`setlocale(LC_ALL, "")` reads `LANG`, `LC_CTYPE`, etc. from the environment.
If the result is a UTF-8 locale (e.g., `en_US.UTF-8`), ncursesw enters
wide-character mode and outputs Unicode box-drawing. Otherwise, it uses
the ACS path.

### 2.4 The `NCURSES_NO_UTF8_ACS` escape hatch

Setting `NCURSES_NO_UTF8_ACS=1` in the environment forces ncursesw to use
ACS even when the locale is UTF-8. This is useful for terminals like PuTTY
whose UTF-8 rendering of box-drawing glyphs is buggy, but whose ACS support
works correctly.

```
export NCURSES_NO_UTF8_ACS=1   # fix q/x/m/j on problematic terminals
```

---

## 3. Terminal Survey

| Environment | TERM value | UTF-8? | ACS works? | Unicode works? | Notes |
|---|---|---|---|---|---|
| Linux console | `linux` | ❌ | ✅ | ❌ | No UTF-8. ACS via terminfo's `acsc` works. `box()` renders correctly. |
| xterm | `xterm`, `xterm-256color` | ✅ | ✅ | ✅ | Both paths work. ncursesw prefers Unicode. |
| tmux | `tmux`, `tmux-256color` | ✅ | ✅ | ✅ | tmux terminfo includes `acsc`. Both paths work. |
| GNU screen | `screen`, `screen-256color` | ✅ | ⚠️ | ✅ | ACS works on modern screen. Older versions may need `NCURSES_NO_UTF8_ACS=1` |
| macOS Terminal | `xterm-256color` | ✅ | ✅ | ✅ | Behaves like xterm. |
| macOS iTerm2 | `xterm-256color` | ✅ | ✅ | ✅ | Behaves like xterm. |
| PuTTY | `xterm`, `putty` | ⚠️ | ✅ | ⚠️ | UTF-8 box-drawing can be buggy. Set `NCURSES_NO_UTF8_ACS=1` |
| Windows Terminal | `xterm-256color` | ✅ | ✅ | ✅ | Modern. Both paths work. |
| Konsole | `xterm-256color` | ✅ | ✅ | ✅ | Both paths work. |
| SSH (any) | forwards client's TERM | ✅ | ✅ | ⚠️ | Depends on SSH client. ACS works. UTF-8 requires locale forwarding. |

**Takeaway**: ACS works on every terminal listed. Unicode box-drawing works
on everything except Linux console and some SSH configurations.

---

## 4. Midnight Commander's Three-Tier Approach

MC uses the most sophisticated rendering strategy found in open-source:

```
                   ┌───────────────────┐
                   │ UTF-8 display?     │
                   │ (nl_langinfo)      │
                   └──────┬────────────┘
                          │
              ┌───────────┴───────────┐
              ▼                       ▼
        UTF-8 locale           8-bit locale
              │                       │
              ▼                       ▼
   ┌─────────────────────┐   ┌──────────────────┐
   │ Unicode via setcchar │   │ ACS via addch()  │
   │ no ACS at all        │   │                   │
   │                      │   │ ACS_* → addch()  │
   │ double lines via     │   │                   │
   │ raw codepoints       │   │ double lines:     │
   │                      │   │ convert via       │
   │                      │   │ iconv, fallback   │
   │                      │   │ to single         │
   └─────────────────────┘   └──────────────────┘
```

MC has additional fallback modes:
- `--ugly` flag: all borders become ASCII `+`, `-`, `|`
- `--slow` flag: all borders become spaces (minimises output)
- "space lines": for very slow serial terminals

---

## 5. htop's Approach

htop skips ncurses ACS entirely. It uses two hardcoded string tables:

```c
// ASCII fallback (always works)
static const char* tree_ascii[] = {
    [VERT] = "|", [RTEE] = "`", [BEND] = "`", [TEND] = ",", [OPEN] = "+",
};

// UTF-8 (when locale permits)
static const char* tree_utf8[] = {
    [VERT] = "\xe2\x94\x82",    // │
    [RTEE] = "\xe2\x94\x9c",    // ├
    [BEND] = "\xe2\x94\x94",    // └
    [TEND] = "\xe2\x94\x8c",    // ┌
    [OPEN] = "\xe2\x94\x80",    // ─
};

// Selection: one line
CRT_treeStr = CRT_utf8 ? CRT_tree_utf8 : CRT_tree_ascii;
```

htop never uses `ACS_*` constants. This is simpler than MC's approach and
works across all environments (ASCII works everywhere, UTF-8 works on all
modern terminals). The cost is: no double-line borders (only single-line).

---

## 6. Recommended Approach for amber

### 6.1 Initialisation sequence (MANDATORY)

```cpp
#include <clocale>
#include <curses.h>

void init_terminal() {
    // 1. Locale — determines ACS vs Unicode path
    // Without this, ncurses assumes ISO-8859-1 and ACS always breaks
    // on modern terminals.
    if (!std::setlocale(LC_ALL, "")) {
        // Fallback for malformed environment locale
        std::setlocale(LC_ALL, "C.UTF-8");
    }
    if (!std::setlocale(LC_ALL, "")) {
        std::setlocale(LC_ALL, "en_US.UTF-8");
    }

    // 2. ncurses initialisation
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);

    // 3. Colours
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
}
```

### 6.2 Border drawing — two-tier approach

```cpp
// Preferred: ncurses ACS constants — ncursesw handles the rest.
// In UTF-8 locale, ncursesw outputs Unicode directly.
// In non-UTF-8 locale, ncurses uses ACS escape sequences.
// Both work correctly on all supported terminals.

void draw_border(WINDOW* win) {
    // box(win, 0, 0) is the canonical ncurses border call.
    // It defaults to ACS_VLINE/ACS_HLINE with ACS_* corners.
    // This is what MC, htop, and most ncurses apps use.
    box(win, 0, 0);

    // For custom title placement:
    // wborder(win, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE,
    //         ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
}

// NEVER do this:
// wborder(win, L'\u2502', L'\u2502', L'\u2500', L'\u2500',
//         L'\u250c', L'\u2510', L'\u2514', L'\u2518');
// Unicode literals bypass ncurses' ACS handling. They produce
// correct output only when locale + terminal both support UTF-8.
// On TERM=linux or with broken locale, they render as garbage.
```

### 6.3 Capability detection

```cpp
struct TermCaps {
    bool color      = false;
    bool utf8       = false;
    bool acs        = false;
    const char* term_type = nullptr;
};

TermCaps detect_capabilities() {
    TermCaps caps;
    caps.term_type = std::getenv("TERM");

    // Colour
    caps.color = has_colors();

    // UTF-8 detection (three methods for robustness)
    const char* lang = std::getenv("LANG");
    const char* lc_all = std::getenv("LC_ALL");
    const char* lc_ctype = std::getenv("LC_CTYPE");

    auto is_utf8 = [](const char* s) -> bool {
        if (!s) return false;
        std::string v(s);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return v.find("utf-8") != std::string::npos ||
               v.find("utf8") != std::string::npos;
    };

    caps.utf8 = is_utf8(lc_all) || is_utf8(lc_ctype) || is_utf8(lang);

    // ACS: check if terminal has acsc in terminfo
    // tigetstr("acs_chars") returns non-null if ACS is available
    caps.acs = tigetstr("acs_chars") != nullptr;

    return caps;
}
```

### 6.4 Fallback strategy

```cpp
class BorderRenderer {
public:
    enum class Style { Unicode, ACS, ASCII };

    void draw_frame(WINDOW* win, Style style) {
        switch (style) {
        case Style::Unicode:
            // ncursesw + UTF-8 locale handles this automatically
            // Just use box() — ncurses does the right thing
        case Style::ACS:
            box(win, 0, 0);  // canonical, works on all terminals
            break;
        case Style::ASCII:
            draw_ascii_border(win);
            break;
        }
    }

private:
    void draw_ascii_border(WINDOW* win) {
        int y, x;
        getmaxyx(win, y, x);
        mvwaddch(win, 0, 0, '+');
        mvwaddch(win, 0, x - 1, '+');
        mvwaddch(win, y - 1, 0, '+');
        mvwaddch(win, y - 1, x - 1, '+');
        mvwhline(win, 0, 1, '-', x - 2);
        mvwhline(win, y - 1, 1, '-', x - 2);
        mvwvline(win, 1, 0, '|', y - 2);
        mvwvline(win, 1, x - 1, '|', y - 2);
    }
};
```

### 6.5 Decision table

| Scenario | TERM | Locale | ACS works? | Unicode works? | Best strategy |
|---|---|---|---|---|---|
| Linux console | `linux` | C/UTF-8 | ✅ | ❌ | ACS (ncurses auto-selects) |
| xterm | `xterm` | UTF-8 | ✅ | ✅ | Unicode (ncurses auto-selects) |
| tmux | `tmux` | UTF-8 | ✅ | ✅ | Unicode (ncurses auto-selects) |
| screen | `screen` | UTF-8 | ✅ | ✅ | Unicode (ncurses auto-selects) |
| macOS Terminal | `xterm-256color` | UTF-8 | ✅ | ✅ | Unicode (ncurses auto-selects) |
| PuTTY | `xterm` | mixed | ✅ | ⚠️ | Unicode, or ACS via `NCURSES_NO_UTF8_ACS=1` |
| SSH to server | client's | forwarded | ✅ | depends | ACS safest, Unicode if locale forwarded |

**In practice**: Just calling `setlocale(LC_ALL, "")` + `initscr()` + `box(win, 0, 0)`
is correct for 95%+ of use cases. ncursesw intelligently picks the right path.
The only common failure is PuTTY with broken UTF-8 box-drawing, which the
user can fix with `NCURSES_NO_UTF8_ACS=1`.

---

## 7. Checklist for amber TUI

- [ ] `setlocale(LC_ALL, "")` called before any ncurses initialisation
- [ ] Fallback to `C.UTF-8` or `en_US.UTF-8` if environment locale is invalid
- [ ] All borders use `box(win, 0, 0)` or explicit `ACS_*` constants
- [ ] Zero uses of raw Unicode box-drawing literals in border code
- [ ] `has_colors()` checked before `start_color()`
- [ ] `use_default_colors()` called so `-1` (terminal default) works
- [ ] `NCURSES_NO_UTF8_ACS=1` documented in the README for problematic terminals
- [ ] Title-in-border uses `ACS_VLINE` separators (not Unicode literals)
- [ ] Fallback to ASCII `+`/`-`/`|` when neither ACS nor Unicode is available

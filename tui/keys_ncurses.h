#ifndef AMBER_TUI_KEYS_NCURSES_H
#define AMBER_TUI_KEYS_NCURSES_H

#include <ncurses.h>

#include "tui/keys.h"

// L4 bridge. Proves that the ncurses codes an adapter reads are the codes the L1
// vocabulary names (tui/keys.h), so passing them through is not an accident.
// If ncurses changes its ABI these asserts fail the build, and the adapter must
// translate ncurses codes into tui::keys values instead (isolation rule 1).
static_assert(tui::keys::kUp == KEY_UP, "ncurses KEY_UP drifted from tui::keys");
static_assert(tui::keys::kDown == KEY_DOWN, "ncurses KEY_DOWN drifted from tui::keys");
static_assert(tui::keys::kLeft == KEY_LEFT, "ncurses KEY_LEFT drifted from tui::keys");
static_assert(tui::keys::kRight == KEY_RIGHT, "ncurses KEY_RIGHT drifted from tui::keys");
static_assert(tui::keys::kHome == KEY_HOME, "ncurses KEY_HOME drifted from tui::keys");
static_assert(tui::keys::kEnd == KEY_END, "ncurses KEY_END drifted from tui::keys");
static_assert(tui::keys::kBackspace == KEY_BACKSPACE,
              "ncurses KEY_BACKSPACE drifted from tui::keys");
static_assert(tui::keys::kDelete == KEY_DC, "ncurses KEY_DC drifted from tui::keys");
static_assert(tui::keys::kNPage == KEY_NPAGE, "ncurses KEY_NPAGE drifted from tui::keys");
static_assert(tui::keys::kPPage == KEY_PPAGE, "ncurses KEY_PPAGE drifted from tui::keys");
static_assert(tui::keys::kEnter == KEY_ENTER, "ncurses KEY_ENTER drifted from tui::keys");
static_assert(tui::keys::kBtab == KEY_BTAB, "ncurses KEY_BTAB drifted from tui::keys");
static_assert(tui::keys::kMouse == KEY_MOUSE, "ncurses KEY_MOUSE drifted from tui::keys");
static_assert(tui::keys::kResize == KEY_RESIZE, "ncurses KEY_RESIZE drifted from tui::keys");
static_assert(tui::keys::kButton1 == BUTTON1_PRESSED,
              "ncurses BUTTON1_PRESSED drifted from tui::keys");
static_assert(tui::keys::kButton4 == BUTTON4_PRESSED,
              "ncurses BUTTON4_PRESSED drifted from tui::keys");
// BUTTON5_PRESSED is absent from macOS's system ncurses; the Homebrew build
// (which AGENTS.md requires) defines it. Assert when present.
#ifdef BUTTON5_PRESSED
static_assert(tui::keys::kButton5 == BUTTON5_PRESSED,
              "ncurses BUTTON5_PRESSED drifted from tui::keys");
#endif

#endif // AMBER_TUI_KEYS_NCURSES_H

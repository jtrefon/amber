#ifndef AMBER_TUI_KEYS_H
#define AMBER_TUI_KEYS_H

#include <cstdint>

// L1 input vocabulary — the port's value space, owned by the domain rather than
// by ncurses. This header is deliberately ncurses-free: L1 modules that need key
// or mouse codes include this, never <ncurses.h>
// (docs/spec/tui/architecture.md, isolation rule 1 / ARCH-01).
//
// The numeric values are the ncurses codes (a stable ABI), so an L4 adapter can
// pass raw getch()/mouse values through without a translation switch.
// tui/keys_ncurses.h static_asserts that equality: if ncurses ever changes its
// ABI the build fails, and the adapter must translate instead of passing codes.
namespace tui::keys {

// Special keys; getch() returns these as int. kNone is the timeout/ERR value.
enum : int {
    kNone = -1,       // getch() timeout (ERR)
    kUp = 259,        // KEY_UP
    kDown = 258,      // KEY_DOWN
    kLeft = 260,      // KEY_LEFT
    kRight = 261,     // KEY_RIGHT
    kHome = 262,      // KEY_HOME
    kEnd = 360,       // KEY_END
    kBackspace = 263, // KEY_BACKSPACE
    kDelete = 330,    // KEY_DC
    kNPage = 338,     // KEY_NPAGE
    kPPage = 339,     // KEY_PPAGE
    kEnter = 343,     // KEY_ENTER
    kMouse = 409,     // KEY_MOUSE
    kResize = 410,    // KEY_RESIZE
};

// Mouse button masks (ncurses BUTTON*_PRESSED values; mmask_t is 32-bit).
using MouseMask = std::uint32_t;
inline constexpr MouseMask kButton1 = 0x2;      // BUTTON1_PRESSED
inline constexpr MouseMask kButton4 = 0x10000;  // BUTTON4_PRESSED — wheel up
inline constexpr MouseMask kButton5 = 0x200000; // BUTTON5_PRESSED — wheel down

} // namespace tui::keys

#endif // AMBER_TUI_KEYS_H

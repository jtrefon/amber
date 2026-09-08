
#ifndef AMBER_TUI_SCROLL_DISPATCH_H
#define AMBER_TUI_SCROLL_DISPATCH_H

#include <ncurses.h>

namespace tui::scroll_dispatch {

// Lines to scroll the chat log for one mouse-wheel event. Wheel-up is
// BUTTON4_PRESSED, wheel-down BUTTON5_PRESSED; any other bstate (arrow keys,
// clicks, drags) maps to 0 so the wheel can never alias keyboard navigation.
int wheel_delta(mmask_t bstate);

// Clamp scroll_top + delta into [0, max_top]. Shared by wheel and PgUp/PgDn
// so every scroll path bounds the viewport identically.
int clamped_scroll_top(int top, int delta, int max_top);

} // namespace tui::scroll_dispatch

#endif // AMBER_TUI_SCROLL_DISPATCH_H

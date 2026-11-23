
#ifndef AMBER_TUI_SCROLL_DISPATCH_H
#define AMBER_TUI_SCROLL_DISPATCH_H

#include "tui/keys.h"

namespace tui::scroll_dispatch {

// Lines to scroll the chat log for one mouse-wheel event. Wheel-up is
// kButton4, wheel-down kButton5; any other mask (arrow keys, clicks, drags)
// maps to 0 so the wheel can never alias keyboard navigation.
int wheel_delta(keys::MouseMask bstate);

// Clamp scroll_top + delta into [0, max_top]. Shared by wheel and PgUp/PgDn
// so every scroll path bounds the viewport identically.
int clamped_scroll_top(int top, int delta, int max_top);

// Whether an appended line should pull the view to the new live edge. The
// answer is the position relative to the PREVIOUS maximum: a reader who
// scrolled up stays where they are, while a view at (or within two lines of)
// the old bottom follows, even when the appended line wraps and the new
// maximum jumps by more than one.
bool follows_live_edge(int scroll_top, int previous_max_top);

} // namespace tui::scroll_dispatch

#endif // AMBER_TUI_SCROLL_DISPATCH_H

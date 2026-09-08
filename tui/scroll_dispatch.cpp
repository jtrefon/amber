
#include "tui/scroll_dispatch.h"

namespace tui::scroll_dispatch {

// RED stubs: wheel never scrolls, clamping is identity — until the real
// mapping lands.
int wheel_delta(mmask_t) { return 0; }

int clamped_scroll_top(int top, int, int) { return top; }

} // namespace tui::scroll_dispatch

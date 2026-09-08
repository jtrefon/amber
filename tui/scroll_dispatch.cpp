
#include "tui/scroll_dispatch.h"

namespace tui::scroll_dispatch {

namespace {
constexpr int kWheelStep = 3;
} // namespace

int wheel_delta(mmask_t bstate) {
    const bool up = (bstate & BUTTON4_PRESSED) != 0;
    const bool down = (bstate & BUTTON5_PRESSED) != 0;
    // Neither, or both buttons at once: no single-direction wheel scroll.
    if (up == down) return 0;
    return up ? -kWheelStep : kWheelStep;
}

int clamped_scroll_top(int top, int delta, int max_top) {
    int t = top + delta;
    if (t < 0) return 0;
    if (t > max_top) return max_top;
    return t;
}

} // namespace tui::scroll_dispatch

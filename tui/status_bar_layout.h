#ifndef AMBER_TUI_STATUS_BAR_LAYOUT_H
#define AMBER_TUI_STATUS_BAR_LAYOUT_H

#include <string>
#include <vector>

#include "agent/extensions.h"

// L1 (pure): status-bar width arbitration. The painter draws what this decides;
// nothing here touches ncurses.
namespace tui::status_bar_layout {

// One registered status segment, as the layout sees it.
struct Segment {
    std::string text;
    int pair = 0;
    int drop = 0;
    agent::StatusAlign align = agent::StatusAlign::Left;
};

// The activity indicator's reserved width (the [ wave ] block).
inline constexpr int kActivityWidth = 13;

// Where the activity wave starts: the first column the left zone may not use,
// with the right zone's separator and its own width already subtracted. Negative
// when the terminal cannot hold the wave. The painter draws from here without a
// further guard, which is what keeps the left zone (clipped at the same column)
// from ever crowding the wave out.
int activity_x(int width, int right_cols);

struct Plan {
    std::vector<Segment> left;  // surviving left-aligned segments, in order
    std::vector<Segment> right; // surviving right-aligned segments, in order
    int right_cols = 0;         // display width of the surviving right zone
    int budget = 0;             // columns the left zone may use
};

// Split by alignment and drop the highest-drop segments until the left zone
// fits `budget`, reserving the right zone and the activity indicator. The
// gauge's minimum width is reserved whenever a context window exists or the
// used count is known, so the bar does not jump when the window is detected
// mid-session.
Plan plan(std::vector<Segment> segments, int width, bool have_ctx, long ctx_used);

} // namespace tui::status_bar_layout

#endif // AMBER_TUI_STATUS_BAR_LAYOUT_H

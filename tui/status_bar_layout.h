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

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

// The scroll badge's value: percent UP from the live edge, because scroll_top
// counts from the top of the buffer (the live edge is max_top). 0 at the live
// edge, 100 at the top of the buffer, -1 when everything fits.
int scroll_pct(int scroll_top, int max_top);

struct Plan {
    std::vector<Segment> left;  // surviving left-aligned segments, in order
    std::vector<Segment> right; // surviving right-aligned segments, in order
    int right_cols = 0;         // display width of the surviving right zone
    int budget = 0;             // columns the left zone may use
};

// Split by alignment and drop the highest-drop segments until the left zone
// fits `budget` -- the whole row minus the right zone and the column that
// separates them. The context gauge always occupies its minimum width: an
// unknown window is a state to show ("ctx —"), not an empty slot, and reserving
// it keeps the bar from jumping when a window is detected mid-session.
Plan plan(std::vector<Segment> segments, int width);

} // namespace tui::status_bar_layout

#endif // AMBER_TUI_STATUS_BAR_LAYOUT_H

#include "tui/status_bar_layout.h"

#include "tui/textutil.h"

namespace tui::status_bar_layout {

namespace {

// A zone's display width, with one column between neighbours.
int zone_cols(const std::vector<Segment>& zone) {
    int c = 0;
    for (size_t i = 0; i < zone.size(); ++i)
        c += text::display_cols(zone[i].text) + (i ? 1 : 0);
    return c;
}

} // namespace

// The right zone is reserved whatever it holds: with the clock switched off the space
// goes back to the left zone instead of staying reserved.
int budget_for(int width, int right_cols) {
    const int reserved = right_cols > 0 ? right_cols + 1 + kActivityWidth : kActivityWidth;
    const int b = width - reserved;
    return b < 0 ? 0 : b;
}

// One drop rule for both zones: the highest drop priority goes first when the bar
// cannot hold everything, wherever the segment attaches. Returns false when nothing
// left may be dropped, which is what ends the loop -- a bar that is still too wide but
// has no droppable segment is left as it is rather than looping forever.
bool drop_worst(std::vector<Segment>& left, std::vector<Segment>& right) {
    std::vector<Segment>* zone = nullptr;
    size_t worst_i = 0;
    int worst = -1;
    for (std::vector<Segment>* z : {&left, &right}) {
        for (size_t i = 0; i < z->size(); ++i) {
            if ((*z)[i].drop > worst) {
                worst = (*z)[i].drop;
                worst_i = i;
                zone = z;
            }
        }
    }
    if (!zone || worst <= 0)
        return false;
    zone->erase(zone->begin() + static_cast<int>(worst_i));
    return true;
}

Plan plan(std::vector<Segment> segments, int width, bool have_ctx, long ctx_used) {
    Plan p;
    for (auto& s : segments)
        (s.align == agent::StatusAlign::Right ? p.right : p.left).push_back(std::move(s));

    // Reserve room for the gauge whether or not the window is known: the count
    // is meaningful on its own ("ctx 44.7k"), the fraction only once a window
    // exists. A constant gauge_min stops the layout from jumping when the window
    // is detected mid-session.
    const int gauge_min = (have_ctx || ctx_used > 0) ? 12 : 0;

    p.right_cols = zone_cols(p.right);
    p.budget = budget_for(width, p.right_cols);

    while (zone_cols(p.left) + gauge_min > p.budget && (!p.left.empty() || !p.right.empty())) {
        if (!drop_worst(p.left, p.right))
            break;
        p.right_cols = zone_cols(p.right);
        p.budget = budget_for(width, p.right_cols);
    }
    return p;
}

} // namespace tui::status_bar_layout

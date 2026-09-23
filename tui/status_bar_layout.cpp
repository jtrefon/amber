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

Plan plan(std::vector<Segment> segments, int width, bool have_ctx, long ctx_used) {
    Plan p;
    for (auto& s : segments)
        (s.align == agent::StatusAlign::Right ? p.right : p.left).push_back(std::move(s));

    // Reserve room for the gauge whether or not the window is known: the count
    // is meaningful on its own ("ctx 44.7k"), the fraction only once a window
    // exists. A constant gauge_min stops the layout from jumping when the window
    // is detected mid-session.
    const int gauge_min = (have_ctx || ctx_used > 0) ? 12 : 0;

    // The right zone is reserved whatever it holds: with the clock switched off
    // the space goes back to the left zone instead of staying reserved.
    const auto budget_for = [width](int right_cols) {
        const int reserved = right_cols > 0 ? right_cols + 1 + kActivityWidth : kActivityWidth;
        const int b = width - reserved;
        return b < 0 ? 0 : b;
    };
    p.right_cols = zone_cols(p.right);
    p.budget = budget_for(p.right_cols);

    // One drop rule for both zones: the highest drop priority goes first when
    // the bar cannot hold everything, wherever the segment attaches.
    while (zone_cols(p.left) + gauge_min > p.budget && (!p.left.empty() || !p.right.empty())) {
        std::vector<Segment>* zone = &p.left;
        int worst = -1, worst_i = -1;
        for (std::vector<Segment>* z : {&p.left, &p.right})
            for (size_t i = 0; i < z->size(); ++i)
                if ((*z)[i].drop > worst) {
                    worst = (*z)[i].drop;
                    worst_i = static_cast<int>(i);
                    zone = z;
                }
        if (worst <= 0)
            break;
        zone->erase(zone->begin() + worst_i);
        p.right_cols = zone_cols(p.right);
        p.budget = budget_for(p.right_cols);
    }
    return p;
}

} // namespace tui::status_bar_layout

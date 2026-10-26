#include "tui/status_bar_layout.h"

#include <algorithm>
#include <cmath>

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

// The left zone may use every column except the right zone and the one that
// separates them. A terminal too narrow for the right zone leaves the left zone
// nothing rather than a negative budget.
int budget_for(int width, int right_cols) {
    const int reserved = right_cols > 0 ? right_cols + 1 : 0;
    return std::max(0, width - reserved);
}

} // namespace

int scroll_pct(int scroll_top, int max_top) {
    if (max_top <= 0)
        return -1;
    if (scroll_top < 0)
        scroll_top = 0;
    if (scroll_top > max_top)
        scroll_top = max_top;
    return static_cast<int>(std::lround(100.0 * (max_top - scroll_top) / max_top));
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

Plan plan(std::vector<Segment> segments, int width) {
    Plan p;
    for (auto& s : segments)
        (s.align == agent::StatusAlign::Right ? p.right : p.left).push_back(std::move(s));

    // The gauge is always present: an unknown window renders "ctx —" (or the
    // live count), so the layout must not reserve zero columns for it. A
    // constant minimum also stops the bar jumping when a window is detected
    // mid-session.
    constexpr int kGaugeMin = 12;

    p.right_cols = zone_cols(p.right);
    p.budget = budget_for(width, p.right_cols);

    while (zone_cols(p.left) + kGaugeMin > p.budget && (!p.left.empty() || !p.right.empty())) {
        if (!drop_worst(p.left, p.right))
            break;
        p.right_cols = zone_cols(p.right);
        p.budget = budget_for(width, p.right_cols);
    }
    return p;
}

} // namespace tui::status_bar_layout

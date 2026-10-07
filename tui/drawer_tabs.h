#pragma once

#include "tui/provider_tabs.h"

#include <string>
#include <vector>

namespace tui {

// The drawer's tab strip, as text.
//
// Rendering lives apart from the strip's content so the layout rules are
// testable without a terminal: which tab is marked, where the counts land, and
// what happens on a narrow screen are all decided here rather than in a paint
// loop.
struct DrawerTabStrip {
    std::string line;
    // Offset of the character that marks the selected tab, for reverse video.
    // npos when nothing is marked (no tabs to show).
    std::size_t selected_mark_pos = std::string::npos;
};

// Render the strip for `tabs` with `selected` active, fitted into `max_width`
// columns. Returns an empty strip when there is nothing to switch between (one
// tab), so the arrow keys keep their normal meaning instead of being claimed for
// a tab bar with one entry.
DrawerTabStrip make_tab_strip(const std::vector<ProviderTab>& tabs, std::size_t selected,
                              int max_width = 200);

} // namespace tui

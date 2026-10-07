#include "tui/drawer_tabs.h"

#include <algorithm>

namespace tui {

namespace {

// How much room one tab may take before the strip gives up on fitting it.
constexpr std::size_t kMaxTabWidth = 18;

std::string truncate(const std::string& s, std::size_t max) {
    if (s.size() <= max)
        return s;
    return s.substr(0, max > 1 ? max - 1 : 0) + "…";
}

} // namespace

DrawerTabStrip make_tab_strip(const std::vector<ProviderTab>& tabs, std::size_t selected,
                              int max_width) {
    DrawerTabStrip strip;
    // One tab is not a tab bar: claiming the arrow keys for a single "All" would
    // silently disable scrolling for nothing.
    if (tabs.size() < 2 || max_width <= 0)
        return strip;

    const std::size_t budget = static_cast<std::size_t>(max_width);
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (!strip.line.empty())
            strip.line += " ";
        const std::size_t mark_at = strip.line.size();
        // The count is what makes a zero-match tab readable rather than
        // mysterious, so it is part of the label, not decoration.
        strip.line += truncate(tabs[i].label, kMaxTabWidth);
        strip.line += "(" + std::to_string(tabs[i].count) + ")";
        if (i == selected)
            strip.selected_mark_pos = mark_at;
        if (strip.line.size() >= budget)
            break;
    }
    if (strip.line.size() > budget)
        strip.line.resize(budget);
    // Truncation can drop the selected marker; a strip that cannot say which tab
    // is active is worse than no strip.
    if (strip.selected_mark_pos != std::string::npos &&
        strip.selected_mark_pos >= strip.line.size()) {
        strip.line.clear();
        strip.selected_mark_pos = std::string::npos;
    }
    return strip;
}

} // namespace tui

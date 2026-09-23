#include "tui/info_dialog_layout.h"

#include <algorithm>

namespace tui::info_dialog_layout {

Layout compute(const std::vector<std::string>& rows, const std::string& title, int screen_h,
               int screen_w) {
    Layout l;
    int maxw = 0;
    for (const auto& r : rows)
        maxw = std::max<int>(maxw, static_cast<int>(r.size()));
    l.width = std::min(screen_w - 4, std::max(maxw + 6, static_cast<int>(title.size()) + 8));
    l.height = std::min(screen_h - 4, static_cast<int>(rows.size()) + 4);
    l.list_h = l.height - 4;

    l.rows = rows;
    for (auto& r : l.rows)
        if (r.empty())
            r = " ";
    return l;
}

ScrollHint scroll_hint(int top_item, int visible, int total) {
    if (total <= visible)
        return {};
    return {true, top_item > 0, top_item + visible < total};
}

} // namespace tui::info_dialog_layout

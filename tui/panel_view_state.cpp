#include "tui/panel_view_state.h"

#include <algorithm>

#include "tui/keys.h"

namespace tui::panel_view_state {

void Scroll::clamp() {
    const int max_top = std::max(0, total - visible);
    top = std::max(0, std::min(top, max_top));
}

int start_index(const std::vector<std::string>& ids, const std::string& start_id) {
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == start_id)
            return static_cast<int>(i);
    return 0;
}

Geometry geometry(int screen_h, int screen_w) {
    Geometry g;
    g.dh = std::max(6, screen_h - 4);
    g.dw = std::max(20, screen_w - 6);
    g.body_h = g.dh - 4;
    g.body_w = g.dw - 4;
    return g;
}

std::vector<std::pair<std::string, std::string>> footer(int panel_count) {
    std::vector<std::pair<std::string, std::string>> f;
    if (panel_count > 1)
        f.emplace_back("Tab", "next panel");
    f.emplace_back("Up/Down", "scroll");
    f.emplace_back("Esc/q", "close");
    return f;
}

std::vector<std::string> visible_rows(const std::vector<std::string>& lines, int width,
                                      const Scroll& scroll) {
    std::vector<std::string> out;
    for (int i = 0; i < scroll.visible; ++i) {
        const int idx = scroll.top + i;
        if (idx >= scroll.total)
            break;
        std::string text = lines[static_cast<std::size_t>(idx)];
        if (static_cast<int>(text.size()) > width)
            text.resize(static_cast<std::size_t>(width));
        out.push_back(std::move(text));
    }
    return out;
}

Arrows arrows(const Scroll& scroll) {
    return {scroll.top > 0, scroll.top + scroll.visible < scroll.total};
}

bool apply_key(int ch, Scroll& scroll) {
    switch (ch) {
    case keys::kUp:
        --scroll.top;
        return true;
    case keys::kDown:
        ++scroll.top;
        return true;
    case keys::kPPage:
        scroll.top -= scroll.visible;
        return true;
    case keys::kNPage:
        scroll.top += scroll.visible;
        return true;
    case keys::kHome:
        scroll.top = 0;
        return true;
    case keys::kEnd:
        scroll.top = scroll.total;
        return true;
    default:
        return false;
    }
}

} // namespace tui::panel_view_state

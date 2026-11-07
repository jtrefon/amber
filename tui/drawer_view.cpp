#include "tui/drawer_view.h"

#include <algorithm>
#include <cstddef>

namespace tui {

namespace {

Style header_style() {
    Style style{Role::Accent};
    style.bold = true;
    return style;
}

Style row_style(bool selected) {
    return selected ? Style{Role::Selected} : Style{Role::Text};
}

} // namespace

Surface paint_drawer(const DrawerView& view, int width) {
    const int columns = std::max(0, width);
    const int height = 1 + static_cast<int>(view.rows.size());
    Surface surface(columns, height);
    surface.clear(Cell{" ", Style{Role::Surface}});
    if (columns == 0)
        return surface;

    const Style header = header_style();
    surface.put_text(0, 0, view.header, header);

    if (!view.tabs.line.empty()) {
        const int x = std::max(0, columns - static_cast<int>(view.tabs.line.size()));
        surface.put_text(x, 0, view.tabs.line, header);
        if (view.tabs.selected_mark_pos != std::string::npos && view.tabs.selected_len > 0) {
            const std::string span =
                view.tabs.line.substr(view.tabs.selected_mark_pos, view.tabs.selected_len);
            surface.put_text(x + static_cast<int>(view.tabs.selected_mark_pos), 0, span,
                             Style{Role::Selected});
        }
    }

    const int selected = std::max(0, std::min(view.selected, height - 2));
    for (int i = 0; i < static_cast<int>(view.rows.size()); ++i) {
        const Style style = row_style(i == selected);
        if (i == selected)
            surface.fill({0, 1 + i, columns, 1}, Cell{" ", style});
        surface.put_text(0, 1 + i, view.rows[static_cast<std::size_t>(i)], style);
    }
    return surface;
}

} // namespace tui

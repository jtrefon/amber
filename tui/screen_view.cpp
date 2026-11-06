#include "tui/screen_view.h"

#include "tui/glyphs.h"
#include "tui/textutil.h"

#include <algorithm>

namespace tui {

namespace {

int centered(int width, const std::string& text) {
    return std::max(0, (width - text_columns(text)) / 2);
}

void paint_row(Surface& surface, const ScreenView& view, int index, int y) {
    if (index < 0 || index >= static_cast<int>(view.rows.size()))
        return;
    const ScreenRow& row = view.rows[static_cast<std::size_t>(index)];
    const Style style = row.header ? Style{Role::Accent}
                                   : (row.selected ? Style{Role::Selected} : Style{Role::Text});
    if (row.selected)
        surface.fill({1, y, surface.width() - 2, 1}, Cell{" ", style});
    surface.put_text(2, y, row.text, style);
}

} // namespace

PaintedScreen paint_screen(const ScreenView& view, Size screen) {
    const int width = std::max(20, screen.w);
    const int height = std::max(6, screen.h);
    const Glyphs& glyphs = text::glyph::utf8() ? Glyphs::unicode() : Glyphs::ascii();
    Surface surface(width, height);
    surface.clear(Cell{" ", Style{Role::Surface}});
    surface.draw_box({0, 0, width, height}, glyphs, Style{Role::Frame});
    surface.put_text(centered(width, view.title), 0, view.title, Style{Role::Title});

    const int footer_y = height - 1;
    const int filter_y = height - 2;
    const int first_y = view.header.empty() ? 1 : 2;
    if (!view.header.empty())
        surface.put_text(2, 1, view.header, Style{Role::Accent});

    const int bottom = filter_y;
    for (int y = first_y; y < bottom; ++y)
        paint_row(surface, view, view.first_row + (y - first_y), y);

    if (view.scroll_up)
        surface.put_text(width - 2, first_y, glyphs.arrow_up, Style{Role::Muted});
    if (view.scroll_down)
        surface.put_text(width - 2, std::max(first_y, bottom - 1), glyphs.arrow_down,
                         Style{Role::Muted});

    int cursor_x = -1;
    if (view.show_filter) {
        const std::string filter = "/ " + view.filter;
        surface.fill({1, filter_y, width - 2, 1}, Cell{" ", Style{Role::Surface}});
        surface.put_text(2, filter_y, filter, Style{Role::Muted});
        cursor_x = 4 + view.filter_cursor;
    }

    if (!view.footer.empty())
        surface.put_text(centered(width, view.footer), footer_y, view.footer, Style{Role::Muted});
    return PaintedScreen{std::move(surface), cursor_x, view.show_filter ? filter_y : -1};
}

} // namespace tui

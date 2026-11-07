#include "tui/text_screen.h"

#include "tui/glyphs.h"
#include "tui/rich_surface.h"
#include "tui/textutil.h"

#include <algorithm>

namespace tui {

namespace {

int centered(int width, const std::string& text) {
    return std::max(0, (width - text_columns(text)) / 2);
}

} // namespace

PaintedScreen paint_text_screen(const TextScreenView& view, Size screen) {
    Surface surface(screen.w, screen.h);
    surface.clear(Cell{" ", Style{Role::Surface}});
    if (screen.w <= 0 || screen.h <= 0)
        return PaintedScreen{std::move(surface), -1, -1};

    const Glyphs& glyphs = text::glyph::utf8() ? Glyphs::unicode() : Glyphs::ascii();
    surface.put_text(centered(screen.w, view.title), 0, view.title, Style{Role::Title});
    surface.draw_hline(0, 1, screen.w, glyphs.hline, Style{Role::Frame});

    const int body_top = 2;
    const int body_height = std::max(0, screen.h - 4);
    const int first = std::max(0, view.top);
    for (int i = 0; i < body_height; ++i) {
        const auto index = static_cast<std::size_t>(first) + static_cast<std::size_t>(i);
        if (index >= view.lines.size())
            break;
        rich::paint_surface_line(surface, 1, body_top + i, screen.w - 2, view.lines[index]);
    }

    std::string footer = view.footer;
    if (!view.search.empty())
        footer = "/" + view.search + "   " + footer;
    if (!footer.empty())
        surface.put_text(1, screen.h - 2, footer, Style{Role::Muted});
    if (!view.position.empty())
        surface.put_text(std::max(1, screen.w - static_cast<int>(view.position.size()) - 1),
                         screen.h - 2, view.position, Style{Role::Muted});
    return PaintedScreen{std::move(surface), -1, -1};
}

} // namespace tui

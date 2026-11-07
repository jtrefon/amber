#include "tui/frame_painter.h"

#include "tui/theme.h"
#include "tui/textutil.h"

#include <algorithm>
#include <type_traits>

namespace tui {

namespace {

std::string utf8_from_wide(const std::wstring& text) {
    std::string out;
    for (const wchar_t wc : text) {
        const auto code =
            static_cast<std::uint32_t>(static_cast<std::make_unsigned_t<wchar_t>>(wc));
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

// Cut at a display-column budget without splitting a UTF-8 glyph.
std::string clip_columns(const std::string& text, int max_columns) {
    if (max_columns < 0)
        return text;
    int used = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t len = std::max<std::size_t>(1, text::utf8_len(text, pos));
        const std::string glyph = text.substr(pos, len);
        const int width = text_columns(glyph);
        if (used + width > max_columns)
            break;
        used += width;
        pos += len;
    }
    return text.substr(0, pos);
}

} // namespace

void FramePainter::begin(Surface& surface) {
    surface_ = &surface;
    x_ = 0;
    y_ = 0;
    attrs_ = 0;
    cursor_visible_ = false;
}

void FramePainter::erase() {
    if (surface_)
        surface_->clear(Cell{" ", Style{Role::Surface}});
}

void FramePainter::move(int y, int x) {
    y_ = y;
    x_ = x;
}

void FramePainter::attrset(int attrs) {
    attrs_ = attrs;
}
void FramePainter::attron(int attrs) {
    attrs_ |= attrs;
}
void FramePainter::attroff(int attrs) {
    attrs_ &= ~attrs;
}

Style FramePainter::style() const {
    const int pair = (attrs_ >> 8) & 0xFF;
    Style style{pair == 0 ? Role::Text : role_for_pair(pair)};
    style.bold = (attrs_ & paint::bold) != 0;
    style.dim = (attrs_ & paint::dim) != 0;
    style.reverse = (attrs_ & paint::reverse) != 0;
    style.underline = (attrs_ & paint::underline) != 0;
    style.italic = (attrs_ & paint::italic) != 0;
    return style;
}

void FramePainter::put(const std::string& text, int max_columns) {
    if (!surface_ || text.empty())
        return;
    const std::string clipped = clip_columns(text, max_columns);
    x_ += surface_->put_text(x_, y_, clipped, style());
}

void FramePainter::addch(char ch) {
    put(std::string(1, ch), -1);
}

void FramePainter::addnstr(const std::string& text, int max_columns) {
    put(text, max_columns);
}

void FramePainter::addnwstr(const std::wstring& text, int max_columns) {
    put(utf8_from_wide(text), max_columns);
}

void FramePainter::mvaddnstr(int y, int x, const std::string& text, int max_columns) {
    move(y, x);
    put(text, max_columns);
}

void FramePainter::mvaddnwstr(int y, int x, const std::wstring& text, int max_columns) {
    move(y, x);
    put(utf8_from_wide(text), max_columns);
}

void FramePainter::clrtoeol() {
    if (!surface_)
        return;
    const int columns = std::max(0, surface_->width() - x_);
    surface_->fill({x_, y_, columns, 1}, Cell{" ", Style{Role::Surface}});
}

void FramePainter::mvhline(int y, int x, char ch, int columns) {
    if (!surface_ || columns <= 0)
        return;
    if (ch == ' ') {
        surface_->fill({x, y, columns, 1}, Cell{" ", style()});
        return;
    }
    for (int i = 0; i < columns; ++i) {
        move(y, x + i);
        put(std::string(1, ch), 1);
    }
}

void FramePainter::blit(const Surface& surface, int x, int y) {
    if (!surface_)
        return;
    for (int row = 0; row < surface.height(); ++row) {
        for (int column = 0; column < surface.width(); ++column) {
            const Cell& cell = surface.at(column, row);
            if (cell.continuation)
                continue;
            surface_->put(x + column, y + row, cell);
        }
    }
}

} // namespace tui

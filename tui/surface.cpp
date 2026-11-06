#include "tui/surface.h"

#include <algorithm>
#include <array>

namespace tui {

namespace {

struct Range {
    std::uint32_t first;
    std::uint32_t last;
};

// Combining marks attach to the preceding glyph and occupy no column.
constexpr std::array<Range, 8> kZeroWidth = {{
    {0x0300, 0x036F},
    {0x1AB0, 0x1AFF},
    {0x1DC0, 0x1DFF},
    {0x20D0, 0x20FF},
    {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F},
    {0xE0100, 0xE01EF},
    {0x200B, 0x200F},
}};

// East Asian wide and emoji ranges: two columns each.
constexpr std::array<Range, 13> kWide = {{
    {0x1100, 0x115F},
    {0x2E80, 0xA4CF},
    {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF},
    {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6},
    {0x1F300, 0x1F64F},
    {0x1F900, 0x1F9FF},
    {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD},
    {0x30000, 0x3FFFD},
}};

bool in(const Range* table, std::size_t count, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (value >= table[i].first && value <= table[i].last)
            return true;
    return false;
}

// Decode one UTF-8 sequence. Invalid input advances one byte and yields U+FFFD,
// so a caller always makes progress.
std::size_t decode(const std::string& text, std::size_t pos, std::uint32_t& codepoint,
                   std::string& glyph) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char first = byte(pos);
    std::size_t length = 1;
    if ((first & 0x80U) == 0) {
        codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
        length = 2;
        codepoint = first & 0x1FU;
    } else if ((first & 0xF0U) == 0xE0U) {
        length = 3;
        codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
        length = 4;
        codepoint = first & 0x07U;
    } else {
        codepoint = 0xFFFD;
        glyph = "?";
        return pos + 1;
    }
    if (pos + length > text.size()) {
        codepoint = 0xFFFD;
        glyph = "?";
        return pos + 1;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = byte(pos + i);
        if ((next & 0xC0U) != 0x80U) {
            codepoint = 0xFFFD;
            glyph = "?";
            return pos + 1;
        }
        codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    glyph = text.substr(pos, length);
    return pos + length;
}

} // namespace

int glyph_width(std::uint32_t codepoint) noexcept {
    if (in(kZeroWidth.data(), kZeroWidth.size(), codepoint))
        return 0;
    if (in(kWide.data(), kWide.size(), codepoint))
        return 2;
    return 1;
}

int text_columns(const std::string& text) noexcept {
    int columns = 0;
    std::uint32_t codepoint = 0;
    std::string glyph;
    for (std::size_t pos = 0; pos < text.size();) {
        pos = decode(text, pos, codepoint, glyph);
        columns += glyph_width(codepoint);
    }
    return columns;
}

Surface::Surface(int width, int height)
    : width_(std::max(0, width)), height_(std::max(0, height)),
      front_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)), back_(front_) {}

const Cell& Surface::at(int x, int y) const {
    static const Cell outside;
    return inside(x, y) ? back_[static_cast<std::size_t>(index(x, y))] : outside;
}

const Cell& Surface::at_front(int x, int y) const {
    static const Cell outside;
    return inside(x, y) ? front_[static_cast<std::size_t>(index(x, y))] : outside;
}

void Surface::clear(const Cell& fill) {
    std::fill(back_.begin(), back_.end(), fill);
    for (Cell& cell : back_)
        cell.continuation = false;
}

int Surface::cell_width(const Cell& cell) const noexcept {
    if (cell.continuation || cell.glyph.empty())
        return 1;
    std::uint32_t codepoint = 0;
    std::string glyph;
    decode(cell.glyph, 0, codepoint, glyph);
    return glyph_width(codepoint);
}

void Surface::repair_wide(int x, int y) {
    if (!inside(x, y))
        return;
    Cell& cell = back_[static_cast<std::size_t>(index(x, y))];
    if (cell.continuation && x > 0) {
        back_[static_cast<std::size_t>(index(x - 1, y))] = Cell{};
        cell = Cell{};
        return;
    }
    if (cell_width(cell) == 2) {
        cell = Cell{};
        if (inside(x + 1, y))
            back_[static_cast<std::size_t>(index(x + 1, y))] = Cell{};
    }
}

void Surface::put(int x, int y, const Cell& cell) {
    if (!inside(x, y))
        return;
    const int width = cell_width(cell);
    if (width == 2 && !inside(x + 1, y))
        return; // never draw half a glyph
    repair_wide(x, y);
    if (width == 2)
        repair_wide(x + 1, y);
    const auto at_index = static_cast<std::size_t>(index(x, y));
    back_[at_index] = cell;
    back_[at_index].continuation = false;
    if (width == 2) {
        back_[static_cast<std::size_t>(index(x + 1, y))] = Cell{"", cell.style, true};
    }
}

int Surface::put_text(int x, int y, const std::string& text, const Style& style) {
    int placed = 0;
    std::uint32_t codepoint = 0;
    std::string glyph;
    for (std::size_t pos = 0; pos < text.size();) {
        pos = decode(text, pos, codepoint, glyph);
        const int width = glyph_width(codepoint);
        if (width == 0) {
            // A combining mark belongs to the glyph just written on this row.
            if (x > 0 && x - 1 < width_ && inside(x - 1, y))
                back_[static_cast<std::size_t>(index(x - 1, y))].glyph += glyph;
            continue;
        }
        if (x >= 0 && x < width_) {
            if (width == 2 && x + 1 >= width_)
                break; // the glyph cannot fit; nothing after it can either
            put(x, y, Cell{glyph, style});
            placed += width;
        }
        x += width;
    }
    return placed;
}

void Surface::fill(const Rect& rect, const Cell& cell) {
    const Rect area = rect.intersect(Rect{0, 0, width_, height_});
    for (int row = area.y; row < area.bottom(); ++row)
        for (int column = area.x; column < area.right(); ++column)
            put(column, row, cell);
}

void Surface::draw_hline(int x, int y, int columns, const char* glyph, const Style& style) {
    const std::string text = glyph ? glyph : "";
    for (int i = 0; i < columns; ++i)
        put_text(x + i, y, text, style);
}

void Surface::draw_box(const Rect& rect, const Glyphs& glyphs, const Style& style) {
    if (rect.w < 2 || rect.h < 2)
        return;
    const int right = rect.right() - 1;
    const int bottom = rect.bottom() - 1;
    for (int x = rect.x + 1; x < right; ++x) {
        put_text(x, rect.y, glyphs.hline, style);
        put_text(x, bottom, glyphs.hline, style);
    }
    for (int y = rect.y + 1; y < bottom; ++y) {
        put_text(rect.x, y, glyphs.vline, style);
        put_text(right, y, glyphs.vline, style);
    }
    put_text(rect.x, rect.y, glyphs.top_left, style);
    put_text(right, rect.y, glyphs.top_right, style);
    put_text(rect.x, bottom, glyphs.bottom_left, style);
    put_text(right, bottom, glyphs.bottom_right, style);
}

std::string Surface::row_text(int y) const {
    if (y < 0 || y >= height_)
        return {};
    std::string text;
    for (int x = 0; x < width_; ++x) {
        const Cell& cell = back_[static_cast<std::size_t>(index(x, y))];
        if (!cell.continuation)
            text += cell.glyph;
    }
    return text;
}

std::vector<Damage> Surface::diff() const {
    std::vector<Damage> damage;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const auto at = static_cast<std::size_t>(index(x, y));
            const Cell& next = back_[at];
            const Cell& shown = front_[at];
            if (next == shown)
                continue;
            if (next.continuation)
                continue; // the lead cell repaints both columns
            // A cell that used to be the second half of a wide glyph is
            // reported too: writing the new value erases the old tail. A blank
            // here would drop whatever new glyph sits in that column.
            damage.push_back(Damage{x, y, next});
        }
    }
    return damage;
}

void Surface::commit() {
    front_.swap(back_);
}

} // namespace tui

#ifndef AMBER_TUI_SURFACE_H
#define AMBER_TUI_SURFACE_H

// A double-buffered cell grid. Painting writes the back buffer; commit() swaps
// it with the front, and diff() names exactly the cells an adapter must repaint
// to make the front match what was just painted. The grid is wide-glyph aware:
// a double-width glyph owns its lead cell and a continuation cell, and the pair
// is repaired together when either half is overwritten.

#include "tui/cell.h"
#include "tui/geometry.h"
#include "tui/glyphs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tui {

struct Damage {
    int x = 0;
    int y = 0;
    Cell cell;
};

// Display columns a Unicode code point occupies: 0 for a combining mark, 2 for
// the East Asian wide/emoji ranges, 1 otherwise. Pure and locale-independent, so
// a headless test and a terminal agree on the grid.
int glyph_width(std::uint32_t codepoint) noexcept;

// Sum of glyph_width over a UTF-8 string.
int text_columns(const std::string& text) noexcept;

class Surface {
public:
    Surface(int width, int height);

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    Size size() const noexcept { return {width_, height_}; }

    // The painted grid: what the next commit will show. Painting code and the
    // compositor read this.
    const Cell& at(int x, int y) const;
    // What the terminal currently shows (the last committed frame).
    const Cell& at_front(int x, int y) const;

    // Reset the back buffer. Painting over the whole surface does not need it;
    // a partial frame that must erase old content does.
    void clear(const Cell& fill = Cell{});

    void put(int x, int y, const Cell& cell);
    // Write UTF-8 text at (x, y), clipping at every edge without splitting a
    // wide glyph. Returns the display columns actually placed.
    int put_text(int x, int y, const std::string& text, const Style& style);

    void fill(const Rect& rect, const Cell& cell);
    void draw_box(const Rect& rect, const Glyphs& glyphs, const Style& style);
    void draw_hline(int x, int y, int columns, const char* glyph, const Style& style);

    // One painted row's glyphs, continuation cells omitted. For tests and
    // debugging.
    std::string row_text(int y) const;

    // Cells whose back differs from front, in row-major order. A stale
    // continuation cell is reported as a blank so the adapter erases the second
    // half of a glyph that was replaced by a single-width cell.
    std::vector<Damage> diff() const;
    void commit();

private:
    bool inside(int x, int y) const noexcept {
        return x >= 0 && x < width_ && y >= 0 && y < height_;
    }
    int index(int x, int y) const noexcept { return (y * width_) + x; }
    void repair_wide(int x, int y);
    int cell_width(const Cell& cell) const noexcept;

    int width_ = 0;
    int height_ = 0;
    std::vector<Cell> front_;
    std::vector<Cell> back_;
};

} // namespace tui

#endif // AMBER_TUI_SURFACE_H

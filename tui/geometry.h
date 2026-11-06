#ifndef AMBER_TUI_GEOMETRY_H
#define AMBER_TUI_GEOMETRY_H

// Pure geometry shared by the cell grid, the compositor and the component base.
// No ncurses, no terminal: a Rect is a half-open box [x, x+w) x [y, y+h), which
// is the same convention ncurses and every clipping algorithm here use.

namespace tui {

struct Point {
    int x = 0;
    int y = 0;
};

struct Size {
    int w = 0;
    int h = 0;
    bool empty() const noexcept { return w <= 0 || h <= 0; }
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int right() const noexcept { return x + w; }
    int bottom() const noexcept { return y + h; }
    bool empty() const noexcept { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const noexcept {
        return px >= x && px < right() && py >= y && py < bottom();
    }
    Rect intersect(const Rect& other) const noexcept {
        const int left = x > other.x ? x : other.x;
        const int top = y > other.y ? y : other.y;
        const int r = right() < other.right() ? right() : other.right();
        const int b = bottom() < other.bottom() ? bottom() : other.bottom();
        if (r <= left || b <= top)
            return {};
        return {left, top, r - left, b - top};
    }
};

} // namespace tui

#endif // AMBER_TUI_GEOMETRY_H

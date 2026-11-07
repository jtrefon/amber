#include "tui/text_canvas.h"

#include "tui/rich_surface.h"

#include <algorithm>

namespace tui {

void TextCanvas::set_viewport(int rows, int cols) {
    const int next_rows = std::max(1, rows);
    const int next_cols = std::max(1, cols);
    if (next_rows == rows_ && next_cols == cols_)
        return;
    const bool width_changed = next_cols != cols_;
    rows_ = next_rows;
    cols_ = next_cols;
    surface_ = Surface(cols_, rows_);
    if (width_changed)
        wrap_dirty_ = true;
    // A dirty wrap is clamped when it is built, so resizing before supplying
    // lines does not wrap the empty canvas just to ask for a bound.
    if (!wrap_dirty_ && top_ > max_top())
        top_ = max_top();
}

void TextCanvas::set_lines(std::vector<rich::Line> lines) {
    if (lines == lines_)
        return; // an unchanged view: keep the wrap and the scroll position
    lines_ = std::move(lines);
    wrap_dirty_ = true;
}

void TextCanvas::ensure_wrapped() {
    if (!wrap_dirty_)
        return;
    wrapped_ = rich::rewrap_all(lines_, cols_);
    wrap_dirty_ = false;
    ++wrap_count_;
    top_ = std::max(0, std::min(top_, std::max(0, static_cast<int>(wrapped_.size()) - rows_)));
}

int TextCanvas::max_top() {
    ensure_wrapped();
    return std::max(0, static_cast<int>(wrapped_.size()) - rows_);
}

int TextCanvas::top() {
    ensure_wrapped();
    return top_;
}

void TextCanvas::set_top(int top) {
    ensure_wrapped();
    top_ = std::max(0, std::min(top, max_top()));
}

int TextCanvas::wrapped_count() {
    ensure_wrapped();
    return static_cast<int>(wrapped_.size());
}

const Surface& TextCanvas::surface() {
    ensure_wrapped();
    surface_.clear(Cell{" ", Style{Role::Surface}});
    const int start = std::min(top_, max_top());
    for (int row = 0; row < rows_; ++row) {
        const int index = start + row;
        if (index < 0 || index >= static_cast<int>(wrapped_.size()))
            continue;
        rich::paint_surface_line(surface_, 0, row, cols_,
                                 wrapped_[static_cast<std::size_t>(index)]);
    }
    return surface_;
}

} // namespace tui

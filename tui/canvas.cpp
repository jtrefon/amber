
#include "canvas.h"

#include <algorithm>
#include <cwchar>

#include "rich_paint.h"
#include "textutil.h"

namespace tui {

Canvas::Canvas() = default;

Canvas::~Canvas() {
    if (win_)
        delwin(win_);
}

void Canvas::resize(int y, int h, int w) {
    if (h < 1)
        h = 1;
    if (w < 1)
        w = 1;
    if (win_ && y_ == y && rows_ == h && cols_ == w)
        return;
    y_ = y;
    rows_ = h;
    cols_ = w;
    if (win_)
        delwin(win_);
    win_ = newwin(h, w, y, 0);
    if (win_) {
        keypad(win_, TRUE);
        // The chat scrollback never holds the cursor; leaving it on lets
        // doupdate() reliably place the physical cursor on the input line
        // (stdscr) instead of snapping it back to the canvas origin.
        leaveok(win_, TRUE);
    }
    rewrap();
}

void Canvas::set_lines(const std::vector<rich::Line>& lines) {
    lines_ = lines;
    rewrap();
}

void Canvas::rewrap() {
    wrapped_ = rich::rewrap_all(lines_, cols_);
    if (top_ > max_top())
        top_ = max_top();
}

void Canvas::draw_hr(const rich::Line& l) {
    // The caller has already positioned the cursor on this line's row; a rule
    // spans the full width rather than starting where the previous write ended.
    const int row = getcury(win_);
    const int pair = l.runs.empty() ? P_BAR_DIM : l.runs[0].pair;
    wattron(win_, COLOR_PAIR(pair));
    mvwhline(win_, row, 0, ACS_HLINE, cols_);
    wattroff(win_, COLOR_PAIR(pair));
}

void Canvas::draw_run(int row, int& x, const rich::Run& r) {
    x = rich::paint_run(win_, row, x, cols_, r);
}

void Canvas::draw_line(int row, const rich::Line& l) {
    if (l.is_hr) {
        wmove(win_, row, 0);
        draw_hr(l);
        return;
    }
    int x = 0;
    for (const auto& r : l.runs) {
        if (x >= cols_)
            break; // nothing left on this row
        draw_run(row, x, r);
    }
}

void Canvas::render() {
    if (!win_)
        return;
    werase(win_);
    const int start = std::min(top_, max_top());
    for (int row = 0; row < rows_; ++row) {
        const int idx = start + row;
        if (idx < 0 || idx >= wrapped_count())
            continue;
        draw_line(row, wrapped_[idx]);
    }
    wnoutrefresh(win_);
}

} // namespace tui

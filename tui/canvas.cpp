
#include "canvas.h"

#include <algorithm>
#include <cwchar>

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

namespace {

// Fit `ws` into `budget` display columns: sets `count` to how many wide chars
// fit and returns the columns consumed. mvwaddnwstr does not clip, so the
// caller must — and by DISPLAY COLUMNS, not wide-char count, because a run may
// hold double-width glyphs (emoji, CJK) that occupy two columns each.
int clamp_to_columns(const std::wstring& ws, int budget, int& count) {
    count = 0;
    int remaining = budget;
    for (wchar_t wc : ws) {
        int w = wcwidth(wc);
        if (w < 0)
            w = 1;
        if (w > remaining)
            break;
        remaining -= w;
        ++count;
    }
    return budget - remaining;
}

} // namespace

// The rule is drawn at the window's cursor (whline takes no row), which is the
// behaviour this has always had.
void Canvas::draw_hr(const rich::Line& l) {
    const int pair = l.runs.empty() ? P_BAR_DIM : l.runs[0].pair;
    wattron(win_, COLOR_PAIR(pair));
    whline(win_, ACS_HLINE, cols_);
    wattroff(win_, COLOR_PAIR(pair));
}

void Canvas::draw_run(int row, int& x, const rich::Run& r) {
    const int room = cols_ - x;
    if (room <= 0)
        return;
    const int attr = (r.bold ? A_BOLD : 0) | (r.dim ? A_DIM : 0) | (r.italic ? A_ITALIC : 0) |
                     (r.under ? A_UNDERLINE : 0);
    wattron(win_, COLOR_PAIR(r.pair) | attr);
    const std::wstring ws = text::to_wide(r.text);
    int n = 0;
    const int drawn = clamp_to_columns(ws, room, n);
    if (n > 0)
        mvwaddnwstr(win_, row, x, ws.c_str(), n);
    wattroff(win_, COLOR_PAIR(r.pair) | attr);
    // Advance by the true drawn width (double-width glyphs consume two
    // columns), not by display_cols() which counts each char as 1.
    x += drawn;
}

void Canvas::draw_line(int row, const rich::Line& l) {
    if (l.is_hr) {
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

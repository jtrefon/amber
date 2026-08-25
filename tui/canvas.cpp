
#include "canvas.h"

#include <algorithm>
#include <cwchar>

#include "textutil.h"

namespace tui {

Canvas::Canvas() = default;

Canvas::~Canvas() {
    if (win_) delwin(win_);
}

void Canvas::resize(int y, int h, int w) {
    if (h < 1) h = 1;
    if (w < 1) w = 1;
    if (win_ && y_ == y && rows_ == h && cols_ == w) return;
    y_ = y; rows_ = h; cols_ = w;
    if (win_) delwin(win_);
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
    if (top_ > max_top()) top_ = max_top();
}

void Canvas::render() {
    if (!win_) return;
    werase(win_);
    int start = std::min(top_, max_top());
    for (int row = 0; row < rows_; ++row) {
        int idx = start + row;
        if (idx < 0 || idx >= wrapped_count()) continue;
        const rich::Line& l = wrapped_[idx];
        if (l.is_hr) {
            wattron(win_, COLOR_PAIR(l.runs.empty() ? P_BAR_DIM : l.runs[0].pair));
            whline(win_, ACS_HLINE, cols_);
            wattroff(win_, COLOR_PAIR(l.runs.empty() ? P_BAR_DIM : l.runs[0].pair));
            continue;
        }
        int x = 0;
        for (const auto& r : l.runs) {
            if (x >= cols_) break;                 // nothing left on this row
            int room = cols_ - x;
            if (room <= 0) break;
            int attr = (r.bold ? A_BOLD : 0) | (r.dim ? A_DIM : 0) |
                       (r.italic ? A_ITALIC : 0) | (r.under ? A_UNDERLINE : 0);
            wattron(win_, COLOR_PAIR(r.pair) | attr);
            std::wstring ws = text::to_wide(r.text);
            // Clamp the write to the window width. mvwaddnwstr does not clip
            // and would otherwise scribble past the row buffer (heap corrup-
            // tion / crash). The clamp must be by DISPLAY COLUMNS, not wide-
            // char count, because a run may contain double-width glyphs (emoji,
            // CJK) that occupy two columns each.
            int budget = room;
            int n = 0;
            for (wchar_t wc : ws) {
                int w = wcwidth(wc);
                if (w < 0) w = 1;
                if (w > budget) break;
                budget -= w;
                ++n;
            }
            if (n > 0)
                mvwaddnwstr(win_, row, x, ws.c_str(), n);
            wattroff(win_, COLOR_PAIR(r.pair) | attr);
            // Advance x by the true drawn width (double-width glyphs consume
            // two columns), not by display_cols() which counts each char as 1.
            x += (room - budget);
        }
    }
    wnoutrefresh(win_);
}

} // namespace tui

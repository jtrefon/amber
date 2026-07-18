// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "canvas.h"

#include <algorithm>

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
    if (win_) keypad(win_, TRUE);
    rewrap();
}

void Canvas::set_lines(const std::vector<rich::Line>& lines) {
    lines_ = lines;
    rewrap();
}

void Canvas::clear_lines() {
    lines_.clear();
    wrapped_.clear();
    top_ = 0;
}

void Canvas::rewrap() {
    wrapped_.clear();
    if (cols_ <= 0) return;
    for (const auto& l : lines_) {
        if (l.is_hr) {
            wrapped_.push_back(l);
            continue;
        }
        auto wl = rich::wrap(l, cols_);
        for (auto& x : wl) wrapped_.push_back(std::move(x));
    }
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
            int cw = text::display_cols(r.text);
            if (x >= cols_) break;                 // nothing left on this row
            int room = cols_ - x;
            int n = std::min(static_cast<int>(r.text.size()), room);
            if (n <= 0) continue;
            int attr = (r.bold ? A_BOLD : 0) | (r.dim ? A_DIM : 0) |
                       (r.italic ? A_ITALIC : 0) | (r.under ? A_UNDERLINE : 0);
            wattron(win_, COLOR_PAIR(r.pair) | attr);
            std::wstring ws = text::to_wide(r.text);
            // Clamp the write to the window width: mvwaddnwstr does not clip
            // and would otherwise scribble past the row buffer (heap corrup-
            // tion / crash) when a run's width plus x exceeds cols_.
            mvwaddnwstr(win_, row, x, ws.c_str(),
                        std::min(static_cast<int>(ws.size()), room));
            x += cw;
            wattroff(win_, COLOR_PAIR(r.pair) | attr);
        }
    }
    wnoutrefresh(win_);
}

} // namespace tui

#ifndef AMBER_TUI_RICH_PAINT_H
#define AMBER_TUI_RICH_PAINT_H

#include <ncurses.h>
#include <wchar.h>

#include "rich.h"
#include "textutil.h"

namespace tui {
namespace rich {

// Fit `ws` into `budget` display columns: sets `count` to how many wide chars
// fit and returns the columns consumed. mvwaddnwstr does not clip, so the
// caller must — and by DISPLAY COLUMNS, not wide-char count, because a run may
// hold double-width glyphs (emoji, CJK) that occupy two columns each.
inline int clamp_columns(const std::wstring& ws, int budget, int& count) {
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

// Paint one run at (row, x), clipped to `cols` columns. Returns the column after
// the last drawn glyph, so a caller can keep appending runs on the same row.
inline int paint_run(WINDOW* w, int row, int x, int cols, const Run& r) {
    const int room = cols - x;
    if (!w || room <= 0)
        return x;
    const int attr = (r.bold ? A_BOLD : 0) | (r.dim ? A_DIM : 0) | (r.italic ? A_ITALIC : 0) |
                     (r.under ? A_UNDERLINE : 0);
    wattron(w, COLOR_PAIR(r.pair) | attr);
    const std::wstring ws = text::to_wide(r.text);
    int n = 0;
    const int drawn = clamp_columns(ws, room, n);
    if (n > 0)
        mvwaddnwstr(w, row, x, ws.c_str(), n);
    wattroff(w, COLOR_PAIR(r.pair) | attr);
    // Advance by the true drawn width (double-width glyphs consume two columns),
    // not by display_cols() which counts each char as 1.
    return x + drawn;
}

// Paint one rich line at `row`, clipped to `cols`. A rule spans the full width
// of its own row.
inline void paint_line(WINDOW* w, int row, int cols, const Line& l) {
    if (!w)
        return;
    if (l.is_hr) {
        const int pair = l.runs.empty() ? P_BAR_DIM : l.runs[0].pair;
        wattron(w, COLOR_PAIR(pair));
        mvwhline(w, row, 0, ACS_HLINE, cols);
        wattroff(w, COLOR_PAIR(pair));
        return;
    }
    int x = 0;
    for (const auto& run : l.runs) {
        if (x >= cols)
            break;
        x = paint_run(w, row, x, cols, run);
    }
}

} // namespace rich
} // namespace tui

#endif // AMBER_TUI_RICH_PAINT_H

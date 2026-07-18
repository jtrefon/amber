// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_RICH_H
#define AMBER_TUI_RICH_H

#include <string>
#include <vector>

#include "widgets.h"   // color pair ids

namespace tui {
namespace rich {

// A contiguous run of text sharing one visual style. `pair` is a widgets.h
// color-pair id; `bold`/`dim`/`italic`/`under` are ncurses attribute flags.
struct Run {
    std::string text;
    int pair = P_ASSISTANT;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool under = false;
};

// One display line, composed of one or more styled runs. A RichLine is the
// atomic unit the canvas lays out: it is wrapped by display-column width into
// one or more physical rows, each inheriting the runs' styles.
struct Line {
    std::vector<Run> runs;
    bool is_code = false;     // rendered in a fenced block (own background)
    bool is_hr = false;       // horizontal rule
    int heading = 0;          // 1..6 for heading lines (extra bold/space)
};

inline bool is_empty(const Line& l) {
    if (l.is_hr) return false;
    for (const auto& r : l.runs)
        if (!r.text.empty()) return false;
    return true;
}

// Number of display columns occupied by a UTF-8 string (whole glyphs count
// as one column). Reused semantics from text::display_cols.
int cols(const std::string& s);

// Word-wrap a single RichLine into physical rows, each a Line carrying the
// same per-run style. Honors display widths so multi-byte glyphs stay aligned
// and are never split mid-character. `width` is the available columns.
std::vector<Line> wrap(const Line& in, int width);

} // namespace rich
} // namespace tui

#endif // AMBER_TUI_RICH_H

#ifndef AMBER_TUI_TEXT_SCREEN_H
#define AMBER_TUI_TEXT_SCREEN_H

// A full-window pager for already-rendered rich lines. The skill body preview
// and the update review are the same surface: a title, a scrollable Markdown
// body, an optional search and a position counter. Painting is pure; the host
// loop that feeds it keys lives beside the caller.

#include "tui/rich.h"
#include "tui/screen_view.h"
#include "tui/surface.h"

#include <string>
#include <vector>

namespace tui {

struct TextScreenView {
    std::string title;
    std::string footer;
    std::string search;
    std::string position;
    std::vector<rich::Line> lines;
    int top = 0;
};

PaintedScreen paint_text_screen(const TextScreenView& view, Size screen);

} // namespace tui

#endif // AMBER_TUI_TEXT_SCREEN_H

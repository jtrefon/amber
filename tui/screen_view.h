#ifndef AMBER_TUI_SCREEN_VIEW_H
#define AMBER_TUI_SCREEN_VIEW_H

// A full-window list screen: one framed Surface with a title, an optional tab
// header, rows, a search row and a footer. Sessions, the Work board and the
// contributed-panel viewer all paint through it, so their frames, selection and
// scrolling cannot drift.

#include "tui/geometry.h"
#include "tui/surface.h"

#include <functional>
#include <string>
#include <vector>

namespace tui {

struct ScreenRow {
    std::string text;
    bool selected = false;
    bool header = false; // a date/section header: emphasised, not selectable
};

struct ScreenView {
    std::string title;
    std::string header;
    std::string footer;
    std::string filter;
    int filter_cursor = 0;
    bool show_filter = false;
    std::vector<ScreenRow> rows;
    int first_row = 0;
    bool scroll_up = false;
    bool scroll_down = false;
};

struct PaintedScreen {
    Surface surface;
    int cursor_x = -1;
    int cursor_y = -1;
};

PaintedScreen paint_screen(const ScreenView& view, Size screen);

// Runs a screen until the handler returns true. The handler may mutate the
// view; the tick runs on an idle timeout (the Work board's live refresh).
class ScreenHost {
public:
    using KeyHandler = std::function<bool(int)>;
    using Tick = std::function<void()>;
    // A custom screen paints its own Surface. The skills explorer needs three
    // panes, so it builds a SkillsScreenView and passes its painter here.
    using Painter = std::function<PaintedScreen(Size)>;

    void run(ScreenView& view, const KeyHandler& handler, int poll_ms = -1, const Tick& tick = {});
    void run(const Painter& painter, const KeyHandler& handler, int poll_ms = -1,
             const Tick& tick = {});
};

} // namespace tui

#endif // AMBER_TUI_SCREEN_VIEW_H

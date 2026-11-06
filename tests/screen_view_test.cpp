// The full-window screen painter: sessions, the Work board and plugin panels
// all use it, so its frame, viewport, selection and search row are pinned once.

#include "tui/screen_view.h"
#include "test_util.h"

#include <string>

namespace {

using tui::PaintedScreen;
using tui::Role;
using tui::ScreenRow;
using tui::ScreenView;

} // namespace

TEST(screen_view_paints_a_full_frame_with_rows_and_a_filter) {
    ScreenView view;
    view.title = "Sessions";
    view.header = "[All]  Sessions  Tasks";
    view.footer = "Enter: load   Esc: back";
    view.filter = "ab";
    view.filter_cursor = 2;
    view.show_filter = true;
    view.scroll_up = true;
    view.scroll_down = true;
    view.rows = {{"date header", false, true}, {"first", false, false}, {"second", true, false}};

    const PaintedScreen painted = tui::paint_screen(view, {80, 24});
    ASSERT(painted.surface.width() == 80 && painted.surface.height() == 24);
    ASSERT(painted.surface.row_text(0).find("Sessions") != std::string::npos);
    ASSERT(painted.surface.row_text(1).find("[All]") != std::string::npos);
    ASSERT(painted.surface.row_text(2).find("date header") != std::string::npos);

    bool selected = false;
    bool header = false;
    for (int x = 0; x < painted.surface.width(); ++x) {
        selected = selected || (painted.surface.at(x, 4).style.role == Role::Selected &&
                                painted.surface.at(x, 4).glyph == std::string("s"));
        header = header || painted.surface.at(x, 2).style.role == Role::Accent;
    }
    ASSERT(selected);
    ASSERT(header);
    ASSERT(painted.surface.row_text(22).find("/ ab") != std::string::npos);
    ASSERT(painted.surface.row_text(23).find("Enter: load") != std::string::npos);
    ASSERT_EQ(painted.cursor_x, 6);
    ASSERT_EQ(painted.cursor_y, 22);
}

TEST(screen_view_clips_to_the_viewport_and_marks_scroll) {
    ScreenView view;
    view.title = "Panel";
    view.rows = {{"zero", false, false},
                 {"one", false, false},
                 {"two", true, false},
                 {"three", false, false},
                 {"four", false, false}};
    view.first_row = 2;
    view.scroll_up = true;
    const PaintedScreen painted = tui::paint_screen(view, {40, 10});
    const std::string screen = painted.surface.row_text(1) + painted.surface.row_text(2) +
                               painted.surface.row_text(3) + painted.surface.row_text(4);
    ASSERT(screen.find("two") != std::string::npos);
    ASSERT(screen.find("four") != std::string::npos);
    ASSERT(screen.find("zero") == std::string::npos);
    ASSERT(painted.cursor_x < 0);

    bool arrow = false;
    for (int x = 0; x < painted.surface.width(); ++x)
        arrow = arrow || painted.surface.at(x, 1).glyph == std::string("\u25b2") ||
                painted.surface.at(x, 1).glyph == std::string("^");
    ASSERT(arrow);
}

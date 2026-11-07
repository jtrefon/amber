// FramePainter is the shell's ncurses-shaped façade over a Surface. These tests
// pin the parts a pty cannot see cheaply: display-column clipping, pair/attribute
// resolution, clear-to-EOL and blitting.

#include "test_util.h"
#include "tui/frame_painter.h"
#include "tui/pairs.h"

#include <string>

namespace {

bool has_role(const tui::Surface& surface, int x, int y, tui::Role role) {
    return surface.at(x, y).style.role == role;
}

} // namespace

TEST(frame_painter_clips_text_by_display_columns) {
    tui::Surface surface(10, 2);
    tui::FramePainter paint;
    paint.begin(surface);
    paint.attrset(tui::paint::color_pair(tui::P_STATUS) | tui::paint::bold);
    paint.addnstr("abcdef", 3);

    ASSERT_EQ(surface.at(0, 0).glyph, std::string("a"));
    ASSERT_EQ(surface.at(1, 0).glyph, std::string("b"));
    ASSERT_EQ(surface.at(2, 0).glyph, std::string("c"));
    ASSERT_EQ(surface.at(3, 0).glyph, std::string(" "));
    ASSERT(has_role(surface, 0, 0, tui::Role::Accent));
    ASSERT(surface.at(0, 0).style.bold);
}

TEST(frame_painter_never_splits_a_wide_glyph) {
    tui::Surface surface(6, 1);
    tui::FramePainter paint;
    paint.begin(surface);
    paint.addnstr("\u754c\u754c", 3); // one full glyph and one column left

    ASSERT_EQ(surface.at(0, 0).glyph, std::string("\u754c"));
    ASSERT(surface.at(1, 0).continuation);
    ASSERT_EQ(surface.at(2, 0).glyph, std::string(" "));
}

TEST(frame_painter_converts_wide_text_and_places_it) {
    tui::Surface surface(6, 2);
    tui::FramePainter paint;
    paint.begin(surface);
    paint.mvaddnwstr(1, 1, L"ab", 2);
    ASSERT_EQ(surface.at(1, 1).glyph, std::string("a"));
    ASSERT_EQ(surface.at(2, 1).glyph, std::string("b"));
}

TEST(frame_painter_clear_to_eol_starts_at_the_cursor) {
    tui::Surface surface(6, 1);
    surface.fill({0, 0, 6, 1}, tui::Cell{"x", tui::Style{tui::Role::Text}});
    tui::FramePainter paint;
    paint.begin(surface);
    paint.move(0, 2);
    paint.clrtoeol();

    ASSERT_EQ(surface.at(1, 0).glyph, std::string("x"));
    ASSERT_EQ(surface.at(2, 0).glyph, std::string(" "));
    ASSERT_EQ(surface.at(5, 0).glyph, std::string(" "));
}

TEST(frame_painter_blits_a_self_contained_surface) {
    tui::Surface target(8, 3);
    tui::FramePainter paint;
    paint.begin(target);

    tui::Surface source(3, 1);
    source.put_text(0, 0, "abc", tui::Style{tui::Role::Selected});
    paint.blit(source, 2, 1);

    ASSERT_EQ(target.at(2, 1).glyph, std::string("a"));
    ASSERT_EQ(target.at(4, 1).glyph, std::string("c"));
    ASSERT(target.at(2, 1).style.role == tui::Role::Selected);
    ASSERT_EQ(target.at(1, 1).glyph, std::string(" "));
}

TEST(frame_painter_default_pair_is_plain_text) {
    tui::Surface surface(4, 1);
    tui::FramePainter paint;
    paint.begin(surface);
    paint.addch('z');
    ASSERT(has_role(surface, 0, 0, tui::Role::Text));
}

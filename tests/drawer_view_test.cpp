// The model picker is the command drawer: rows from the command tree, a header
// strip, and provider tabs. These tests pin the painter's placement and styles
// at the sizes the drawer actually opens at.

#include "test_util.h"
#include "tui/drawer_view.h"

#include <string>

namespace {

bool row_has_role(const tui::Surface& surface, int y, tui::Role role, const std::string& glyph) {
    for (int x = 0; x < surface.width(); ++x) {
        if (surface.at(x, y).glyph == glyph && surface.at(x, y).style.role == role)
            return true;
    }
    return false;
}

std::string row_text(const tui::Surface& surface, int y) {
    return surface.row_text(y);
}

} // namespace

TEST(drawer_view_paints_the_header_tabs_and_the_selected_row) {
    tui::DrawerView view;
    view.header = " options  (Tab complete) ";
    view.tabs.line = "All(3) acme(2)";
    view.tabs.selected_mark_pos = 7; // "acme(2)"
    view.tabs.selected_len = 7;
    view.rows = {"  alpha", "  beta", "  gamma"};
    view.selected = 1;

    const tui::Surface surface = tui::paint_drawer(view, 40);
    ASSERT_EQ(surface.width(), 40);
    ASSERT_EQ(surface.height(), 4);
    ASSERT(row_text(surface, 0).find("options") != std::string::npos);
    ASSERT(row_text(surface, 0).find("acme(2)") != std::string::npos);
    ASSERT(row_has_role(surface, 0, tui::Role::Accent, "o"));
    ASSERT(row_has_role(surface, 0, tui::Role::Selected, "a"));

    ASSERT(row_text(surface, 1).find("alpha") != std::string::npos);
    ASSERT(row_text(surface, 2).find("beta") != std::string::npos);
    // The whole selected row is a bar, not only its text.
    for (int x = 0; x < surface.width(); ++x)
        ASSERT(surface.at(x, 2).style.role == tui::Role::Selected);
    ASSERT(surface.at(39, 2).glyph == std::string(" "));
    ASSERT(row_text(surface, 3).find("gamma") != std::string::npos);
}

TEST(drawer_view_clips_a_narrow_screen_without_wrapping) {
    tui::DrawerView view;
    view.header = " options  (Tab complete) ";
    view.tabs.line = "All(30) averylongprovider(12)";
    view.rows = {"  a very long model name that will not fit", "  short"};
    view.selected = 0;

    const tui::Surface surface = tui::paint_drawer(view, 12);
    ASSERT_EQ(surface.width(), 12);
    ASSERT_EQ(surface.height(), 3);
    for (int y = 0; y < surface.height(); ++y) {
        const std::string row = row_text(surface, y);
        ASSERT(static_cast<int>(row.size()) <= 12);
    }
    // The selected row still fills the width; clipping must not leave a gap.
    for (int x = 0; x < 12; ++x)
        ASSERT(surface.at(x, 1).style.role == tui::Role::Selected);
}

TEST(drawer_view_with_no_rows_is_just_the_header) {
    tui::DrawerView view;
    view.header = " options ";
    const tui::Surface surface = tui::paint_drawer(view, 20);
    ASSERT_EQ(surface.height(), 1);
    ASSERT(row_text(surface, 0).find("options") != std::string::npos);
}

TEST(drawer_view_tabs_win_over_the_header_text_where_they_meet) {
    tui::DrawerView view;
    view.header = " options  (Tab complete) ";
    view.tabs.line = "All(3)";
    view.tabs.selected_mark_pos = 0;
    view.tabs.selected_len = 6;
    const tui::Surface surface = tui::paint_drawer(view, 20);
    // Right-aligned, so the strip starts at column 14 and covers the header's
    // last cells; the selected tab is the only Selected span on the row.
    ASSERT(row_text(surface, 0).find("All(3)") != std::string::npos);
    ASSERT(row_has_role(surface, 0, tui::Role::Selected, "A"));
}

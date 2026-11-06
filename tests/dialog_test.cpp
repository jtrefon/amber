// The modal layer is two pure halves: a state machine per sheet and a painter
// that turns one view-model into a framed Surface. These tests pin the key
// contracts the old libform/libmenu dialogs had — Enter accepts, Esc cancels,
// the form's Tab/arrow map, the countdown's fail-safe — and the geometry the
// host blits.

#include "tui/dialog_state.h"
#include "tui/dialog_view.h"
#include "tui/keys.h"
#include "test_util.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {

using tui::ConfirmState;
using tui::DialogKey;
using tui::DialogView;
using tui::FieldSpec;
using tui::FormState;
using tui::InfoState;
using tui::PaintedDialog;
using tui::Role;

bool has_role(const tui::Surface& surface, Role role) {
    for (int y = 0; y < surface.height(); ++y)
        for (int x = 0; x < surface.width(); ++x)
            if (surface.at(x, y).style.role == role)
                return true;
    return false;
}

} // namespace

TEST(confirm_state_defaults_to_no_and_resolves_with_enter) {
    ConfirmState state;
    ASSERT_FALSE(state.yes());
    ASSERT(state.key('\t') == DialogKey::Redraw);
    ASSERT(state.yes());
    ASSERT(state.key('\t') == DialogKey::Redraw);
    ASSERT_FALSE(state.yes());
    ASSERT(state.key('\t') == DialogKey::Redraw);
    ASSERT(state.key('\n') == DialogKey::Accept);
    ASSERT(state.yes());
}

TEST(confirm_state_escape_is_cancel) {
    ConfirmState state;
    ASSERT(state.key(27) == DialogKey::Cancel);
    ASSERT_FALSE(state.yes());
}

TEST(info_state_scrolls_within_the_content_bounds) {
    InfoState state;
    ASSERT(state.key(tui::keys::kDown, 3, 10) == DialogKey::Redraw);
    ASSERT_EQ(state.top(), 1);
    ASSERT(state.key(tui::keys::kNPage, 3, 10) == DialogKey::Redraw);
    ASSERT_EQ(state.top(), 3);
    ASSERT(state.key(tui::keys::kEnd, 3, 10) == DialogKey::Redraw);
    ASSERT_EQ(state.top(), 7);
    ASSERT(state.key(tui::keys::kDown, 3, 10) == DialogKey::None);
    ASSERT(state.key(tui::keys::kUp, 3, 10) == DialogKey::Redraw);
    ASSERT_EQ(state.top(), 6);
    ASSERT(state.key(tui::keys::kHome, 3, 10) == DialogKey::Redraw);
    ASSERT_EQ(state.top(), 0);
    ASSERT(state.key('\n', 3, 10) == DialogKey::Accept);
}

TEST(form_state_edits_the_focused_field) {
    FormState state({FieldSpec{"name", "am", false}, FieldSpec{"model", "x", false}});
    ASSERT(state.on_field());
    ASSERT_EQ(state.field_index(), 0u);
    ASSERT_EQ(state.cursor(), 2u);

    ASSERT(state.key('b') == DialogKey::Redraw);
    ASSERT(state.key('e') == DialogKey::Redraw);
    ASSERT(state.key('r') == DialogKey::Redraw);
    ASSERT_EQ(state.fields()[0].value, std::string("amber"));
    ASSERT_EQ(state.cursor(), 5u);

    ASSERT(state.key(tui::keys::kLeft) == DialogKey::Redraw);
    ASSERT(state.key(tui::keys::kBackspace) == DialogKey::Redraw);
    ASSERT_EQ(state.fields()[0].value, std::string("ambr"));
    ASSERT(state.key(tui::keys::kHome) == DialogKey::Redraw);
    ASSERT_EQ(state.cursor(), 0u);
    ASSERT(state.key(tui::keys::kDelete) == DialogKey::Redraw);
    ASSERT_EQ(state.fields()[0].value, std::string("mbr"));
    ASSERT(state.key(tui::keys::kEnd) == DialogKey::Redraw);

    ASSERT(state.key('\t') == DialogKey::Redraw);
    ASSERT_EQ(state.field_index(), 1u);
    ASSERT_EQ(state.cursor(), 1u);
    ASSERT(state.key('!') == DialogKey::Redraw);
    ASSERT_EQ(state.fields()[1].value, std::string("x!"));
}

TEST(form_state_paste_inserts_at_the_cursor_and_skips_controls) {
    FormState state({FieldSpec{"key", "a", false}});
    state.insert_text("b\r\n\tc");
    ASSERT_EQ(state.fields()[0].value, std::string("abc"));
    ASSERT_EQ(state.cursor(), 3u);
}

TEST(form_state_tab_reaches_the_buttons_and_enter_decides) {
    FormState state({FieldSpec{"name", "amber", false}});
    ASSERT(state.key('\t') == DialogKey::Redraw);
    ASSERT(state.ok_focused());
    ASSERT(state.key('\t') == DialogKey::Redraw);
    ASSERT(state.cancel_focused());
    ASSERT(state.key('\t') == DialogKey::Redraw);
    ASSERT(state.ok_focused());
    ASSERT(state.key('\n') == DialogKey::Accept);
    ASSERT(state.accepted());

    FormState cancel({FieldSpec{"name", "amber", false}});
    ASSERT(cancel.key(27) == DialogKey::Cancel);
    ASSERT(cancel.cancelled());
}

TEST(dialog_view_frames_a_message_at_both_target_sizes) {
    for (const tui::Size screen : {tui::Size{80, 24}, tui::Size{120, 40}}) {
        DialogView view;
        view.title = "Confirm";
        view.lines = {"Delete this session?"};
        view.buttons = {"[ Yes ]", "[ No ]"};
        view.selected_button = 1;
        view.footer = "Enter: confirm   Esc: cancel";

        const PaintedDialog painted = tui::paint_dialog(view, screen);
        ASSERT(painted.x >= 0 && painted.y >= 0);
        ASSERT(painted.x + painted.surface.width() <= screen.w);
        ASSERT(painted.y + painted.surface.height() <= screen.h);
        ASSERT(painted.surface.width() >= 40);
        ASSERT(painted.surface.at(0, 0).glyph == std::string("┌") ||
               painted.surface.at(0, 0).glyph == std::string("+"));
        ASSERT(has_role(painted.surface, Role::Selected));
        ASSERT(has_role(painted.surface, Role::Muted));

        bool title = false;
        for (int y = 0; y < painted.surface.height(); ++y)
            title = title || painted.surface.row_text(y).find("Confirm") != std::string::npos;
        ASSERT(title);
    }
}

TEST(dialog_view_list_marks_the_selection_and_the_filter) {
    DialogView view;
    view.title = "Pick";
    view.items = {"alpha", "beta", "gamma"};
    view.selected_item = 2;
    view.filter = "ga";
    view.filter_mode = true;
    const PaintedDialog painted = tui::paint_dialog(view, {80, 24});

    bool selected = false;
    bool filter = false;
    for (int y = 0; y < painted.surface.height(); ++y) {
        for (int x = 0; x < painted.surface.width(); ++x)
            selected = selected || (painted.surface.at(x, y).style.role == Role::Selected &&
                                    painted.surface.at(x, y).glyph == std::string("g"));
        filter = filter || painted.surface.row_text(y).find("/ ga") != std::string::npos;
    }
    ASSERT(selected);
    ASSERT(filter);
}

TEST(dialog_view_form_shows_the_value_and_places_the_cursor) {
    DialogView view;
    view.title = "API key";
    view.fields = {FieldSpec{"key", "sk-test", false}};
    view.focused_field = 0;
    view.field_cursor = 3;
    view.buttons = {"[  OK  ]", "[ Cancel ]"};
    view.selected_button = -1;
    const PaintedDialog painted = tui::paint_dialog(view, {80, 24});

    ASSERT(painted.cursor_x > 0);
    ASSERT(painted.cursor_y > 0);
    ASSERT(painted.surface.at(painted.cursor_x, painted.cursor_y).style.role == Role::Selected);
    bool value = false;
    for (int y = 0; y < painted.surface.height(); ++y)
        value = value || painted.surface.row_text(y).find("sk-test") != std::string::npos;
    ASSERT(value);
}

TEST(dialog_view_notice_is_an_error_and_buttons_are_drawn) {
    DialogView view;
    view.title = "Provider";
    view.fields = {FieldSpec{"name", "acme", false}};
    view.notice = "the endpoint rejected this key";
    view.buttons = {"[  OK  ]", "[ Cancel ]"};
    view.selected_button = 0;
    const PaintedDialog painted = tui::paint_dialog(view, {100, 30});
    ASSERT(has_role(painted.surface, Role::Error));
    bool buttons = false;
    for (int y = 0; y < painted.surface.height(); ++y)
        buttons = buttons || painted.surface.row_text(y).find("[ Cancel ]") != std::string::npos;
    ASSERT(buttons);
}

TEST(dialog_view_clamps_to_a_small_screen) {
    DialogView view;
    view.title = "A very long dialog title that cannot fit";
    view.lines = {"a long body line that has to wrap several times at this width"};
    const PaintedDialog painted = tui::paint_dialog(view, {30, 10});
    ASSERT(painted.surface.width() <= 30);
    ASSERT(painted.surface.height() <= 10);
    ASSERT(painted.x >= 0 && painted.y >= 0);
    ASSERT(painted.x + painted.surface.width() <= 30);
}

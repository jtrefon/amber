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

TEST(confirm_state_accepts_y_and_n) {
    ConfirmState yes;
    ASSERT(yes.key('y') == DialogKey::Accept);
    ASSERT(yes.yes());

    ConfirmState no;
    ASSERT(no.key('n') == DialogKey::Cancel);
    ASSERT_FALSE(no.yes());
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

// A secret field is seeded with the credential already on file. That seed is a
// placeholder, not text the user is editing: typing or pasting a replacement
// must produce exactly the new credential. Inserting at the end gave a provider
// file with two keys joined by spaces, which the gateway rejected and the key
// prompt then offered again.
TEST(form_state_pasting_into_a_seeded_secret_replaces_the_seed) {
    FormState state({FieldSpec{"API Key", "old-key", true}});
    state.insert_text("new-key");
    ASSERT_EQ(state.fields()[0].value, std::string("new-key"));
    ASSERT_EQ(state.cursor(), 7u);

    // Once the placeholder is gone, later edits behave like a normal field.
    state.insert_text("-tail");
    ASSERT_EQ(state.fields()[0].value, std::string("new-key-tail"));
}

TEST(form_state_tab_then_paste_replaces_the_secret_seed) {
    FormState state({FieldSpec{"Server URL", "http://x", false},
                     FieldSpec{"API Key", "sk-old-key", true}, FieldSpec{"Model", "", false}});
    ASSERT(state.key('\t') == DialogKey::Redraw);
    ASSERT_EQ(state.field_index(), 1u);
    state.insert_text("sk-new-key");
    ASSERT_EQ(state.fields()[1].value, std::string("sk-new-key"));
}

TEST(form_state_typing_into_a_seeded_secret_replaces_the_seed) {
    FormState state({FieldSpec{"API Key", "old-key", true}});
    ASSERT(state.key('x') == DialogKey::Redraw);
    ASSERT_EQ(state.fields()[0].value, std::string("x"));
    ASSERT_EQ(state.cursor(), 1u);
}

TEST(form_state_backspace_clears_a_seeded_secret) {
    FormState state({FieldSpec{"API Key", "old-key", true}});
    ASSERT(state.key(tui::keys::kBackspace) == DialogKey::Redraw);
    ASSERT_EQ(state.fields()[0].value, std::string());
}

// A token copied from a wrapped display often carries spaces or a newline at
// its edges. Those must not become part of the credential that is persisted.
TEST(form_state_pasting_a_wrapped_secret_trims_its_edges) {
    FormState state({FieldSpec{"API Key", "old-key", true}});
    state.insert_text("  new-key \r\n\t");
    ASSERT_EQ(state.fields()[0].value, std::string("new-key"));
    ASSERT_EQ(state.cursor(), 7u);
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
        view.footer_keys = {{"Enter", "Confirm"}, {"Esc", "Cancel"}};

        const PaintedDialog painted = tui::paint_dialog(view, screen);
        ASSERT(painted.x >= 0 && painted.y >= 0);
        ASSERT(painted.x + painted.surface.width() <= screen.w);
        ASSERT(painted.y + painted.surface.height() <= screen.h);
        ASSERT(painted.surface.width() >= 40);
        ASSERT(painted.surface.at(0, 0).glyph == std::string("┌") ||
               painted.surface.at(0, 0).glyph == std::string("+"));
        ASSERT(has_role(painted.surface, Role::DialogButtonFocus));
        ASSERT(has_role(painted.surface, Role::DialogFrame));
        ASSERT(has_role(painted.surface, Role::DialogTitle));
        ASSERT(has_role(painted.surface, Role::DialogFooterKey));

        bool title = false;
        for (int y = 0; y < painted.surface.height(); ++y)
            title = title || painted.surface.row_text(y).find("Confirm") != std::string::npos;
        ASSERT(title);
        const std::string top = painted.surface.row_text(0);
        ASSERT(top.find("┤") != std::string::npos); // ┤
        ASSERT(top.find("├") != std::string::npos); // ├
        const std::string bottom = painted.surface.row_text(painted.surface.height() - 2);
        ASSERT(bottom.find("[Esc]") != std::string::npos);
        ASSERT(bottom.find("Cancel") != std::string::npos);
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
            selected = selected || (painted.surface.at(x, y).style.role == Role::DialogSelected &&
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
    ASSERT(painted.surface.at(painted.cursor_x, painted.cursor_y).style.role ==
           Role::DialogSelected);
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
    ASSERT(has_role(painted.surface, Role::DialogError));
    bool buttons = false;
    for (int y = 0; y < painted.surface.height(); ++y)
        buttons = buttons || painted.surface.row_text(y).find("[ Cancel ]") != std::string::npos;
    ASSERT(buttons);
}

TEST(dialog_view_shadow_is_visible) {
    DialogView view;
    view.title = "Confirm";
    view.lines = {"Delete it?"};
    view.buttons = {"[ Yes ]", "[ No ]"};
    const PaintedDialog painted = tui::paint_dialog(view, {80, 24});

    bool shadow = false;
    for (int y = 1; y < painted.surface.height(); ++y) {
        const tui::Cell& cell = painted.surface.at(painted.surface.width() - 1, y);
        if (cell.style.role == Role::Shadow && cell.glyph != " ")
            shadow = true;
    }
    for (int x = 1; x < painted.surface.width(); ++x) {
        const tui::Cell& cell = painted.surface.at(x, painted.surface.height() - 1);
        if (cell.style.role == Role::Shadow && cell.glyph != " ")
            shadow = true;
    }
    ASSERT(shadow);
    ASSERT(painted.surface.at(0, 0).style.role == Role::DialogFrame);
    ASSERT(painted.surface.at(1, 1).style.role == Role::DialogSurface);
}

TEST(approval_sheet_is_vertical_and_stable_at_80x24) {
    const auto make = [](const std::string& status) {
        DialogView view;
        view.title = "Approve action?";
        view.lines = {"[main] bash: rm -rf build/"};
        view.options = {{"1", "Allow once"},
                        {"2", "Allow session"},
                        {"3", "Always allow"},
                        {"4", "Always deny"}};
        view.selected_option = 1;
        view.status = status;
        view.footer_keys = {
            {"1-4", "Pick"}, {"Tab", "Move"}, {"Enter", "Confirm"}, {"Esc", "Cancel"}};
        view.preferred_width = 62;
        return tui::paint_dialog(view, {80, 24});
    };
    const PaintedDialog sixty = make("auto-deny in 60s");
    const PaintedDialog nine = make("auto-deny in 9s");

    ASSERT_EQ(sixty.surface.width(), nine.surface.width());
    ASSERT_EQ(sixty.x, nine.x);
    ASSERT_EQ(sixty.y, nine.y);
    ASSERT(sixty.surface.width() <= 78);
    ASSERT(sixty.surface.width() >= 58);

    int option_rows = 0;
    int countdown_rows = 0;
    bool selected_bar = false;
    for (int y = 0; y < sixty.surface.height(); ++y) {
        const std::string row = sixty.surface.row_text(y);
        if (row.find("Allow once") != std::string::npos)
            ++option_rows;
        if (row.find("Allow session") != std::string::npos) {
            ++option_rows;
            selected_bar = true;
            for (int x = 1; x < sixty.surface.width() - 2; ++x)
                selected_bar =
                    selected_bar && sixty.surface.at(x, y).style.role == Role::DialogSelected;
        }
        if (row.find("Always allow") != std::string::npos)
            ++option_rows;
        if (row.find("Always deny") != std::string::npos)
            ++option_rows;
        if (row.find("auto-deny in") != std::string::npos)
            ++countdown_rows;
    }
    ASSERT_EQ(option_rows, 4);
    ASSERT_EQ(countdown_rows, 1);
    ASSERT(selected_bar);
    ASSERT(sixty.surface.row_text(0).find("Approve action?") != std::string::npos);
    ASSERT(sixty.surface.row_text(0).find("\u2524") != std::string::npos);
    ASSERT(sixty.surface.row_text(sixty.surface.height() - 2).find("[Esc]") != std::string::npos);
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

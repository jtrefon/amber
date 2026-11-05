// The menu tree, its selection state and the painted rectangle model. Pure L1:
// the renderer and the mouse handler both use the same layout.

#include "agent/data_path.h"
#include "tui/key_binder.h"
#include "tui/menu_controller.h"
#include "tui/menu_model.h"
#include "tui/menu_view.h"
#include "tui/keys.h"
#include "test_util.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <string>

namespace {

using tui::menu::Item;
using tui::menu::Mode;
using tui::menu::Model;
using tui::menu::State;

Model sample_model() {
    Model model;
    const auto json = nlohmann::json::parse(R"({
      "menus": [
        {"label": "File", "items": [
          {"label": "New session", "action": "core.window.new"},
          {"separator": true},
          {"label": "Theme", "children": [
            {"label": "Classic", "action": "ui.theme.classic"},
            {"label": "Modern", "action": "ui.theme.modern"}
          ]},
          {"label": "Quit", "action": "core.quit"}
        ]},
        {"label": "Help", "items": [
          {"label": "Keys", "action": "core.help"}
        ]}
      ]
    })");
    ASSERT(model.parse(json));
    return model;
}

Model submenu_model() {
    Model model;
    const auto json = nlohmann::json::parse(R"({
      "menus": [
        {"label": "File", "items": [
          {"label": "Theme", "children": [
            {"label": "Classic", "action": "ui.theme.classic"},
            {"separator": true},
            {"label": "Modern", "action": "ui.theme.modern"}
          ]},
          {"label": "Empty", "children": [{"separator": true}]},
          {"label": "Leaf", "action": "core.leaf"},
          {"label": "Nested", "children": [
            {"label": "Group", "children": [
              {"label": "Deep", "action": "core.deep"}
            ]}
          ]}
        ]}
      ]
    })");
    ASSERT(model.parse(json));
    return model;
}

Model tall_submenu_model() {
    Model model;
    const auto json = nlohmann::json::parse(R"({
      "menus": [
        {"label": "File", "items": [
          {"label": "One", "action": "one"},
          {"label": "Two", "action": "two"},
          {"label": "Three", "action": "three"},
          {"label": "Four", "action": "four"},
          {"label": "Five", "action": "five"},
          {"label": "Sub", "children": [{"label": "Only", "action": "only"}]},
          {"label": "Seven", "action": "seven"},
          {"label": "Eight", "action": "eight"}
        ]}
      ]
    })");
    ASSERT(model.parse(json));
    return model;
}

void collect_actions(const nlohmann::json& json, std::set<std::string>& out) {
    if (json.is_object()) {
        if (json.contains("action") && json["action"].is_string())
            out.insert(json["action"].get<std::string>());
        for (const auto& [key, value] : json.items())
            if (key != "action")
                collect_actions(value, out);
    } else if (json.is_array()) {
        for (const auto& value : json)
            collect_actions(value, out);
    }
}

} // namespace

TEST(menu_model_parses_labels_separators_and_children) {
    const Model model = sample_model();
    ASSERT_EQ(model.menus.size(), 2u);
    ASSERT_EQ(model.menus[0].label, std::string("File"));
    ASSERT_EQ(model.menus[0].items.size(), 4u);
    ASSERT_TRUE(model.menus[0].items[1].separator);
    ASSERT_EQ(model.menus[0].items[2].children.size(), 2u);
    ASSERT_EQ(model.menus[0].items[2].children[0].label, std::string("Classic"));
}

TEST(menu_model_rejects_a_menu_without_items) {
    Model model;
    ASSERT_FALSE(model.parse(nlohmann::json::parse(R"({"menus":[{"label":"Empty"}]})")));
}

TEST(menu_model_rejects_malformed_shapes) {
    Model model;
    ASSERT_FALSE(model.parse(nlohmann::json(42)));
    ASSERT_FALSE(model.parse(nlohmann::json::object()));
    ASSERT_FALSE(model.parse(nlohmann::json::parse(R"({"menus":{}})")));
    ASSERT_FALSE(model.parse(nlohmann::json::parse(R"({"menus":[42]})")));
    ASSERT_FALSE(model.parse(nlohmann::json::parse(R"({"menus":[{"label":"F","items":[42]}]})")));
    ASSERT_FALSE(model.parse(
        nlohmann::json::parse(R"({"menus":[{"label":"F","items":[{"action":"a"}]}]})")));
    ASSERT_FALSE(model.parse(nlohmann::json::parse(
        R"({"menus":[{"label":"F","items":[{"label":"A","children":{}}]}]})")));
    ASSERT_FALSE(model.parse(nlohmann::json::parse(
        R"({"menus":[{"label":"F","items":[{"label":"A","children":[42]}]}]})")));
    ASSERT_FALSE(
        model.parse(nlohmann::json::parse(R"({"menus":[{"label":"F","items":[{"label":"A"}]}]})")));
    ASSERT_FALSE(model.parse(nlohmann::json::parse(R"({"menus":[]})")));
    ASSERT_FALSE(model.parse(nlohmann::json::parse(R"({"menus":[{"label":"F","items":[]}]})")));
}

TEST(menu_model_load_reports_missing_and_malformed_files) {
    Model model;
    ASSERT_FALSE(model.load("/tmp/amber-menu-does-not-exist.json"));

    const std::string path = "/tmp/amber-menu-malformed-" + std::to_string(::getpid()) + ".json";
    {
        std::ofstream out(path);
        out << "{ this is not json";
    }
    ASSERT_FALSE(model.load(path));
    std::remove(path.c_str());
}

TEST(menu_navigation_guards_cover_closed_and_empty_menus) {
    const Model model = sample_model();
    State state;
    tui::menu::move_menu(state, model, 1); // closed: no-op
    ASSERT(state.mode == Mode::Closed);

    Model separators_only;
    ASSERT(separators_only.parse(
        nlohmann::json::parse(R"({"menus":[{"label":"File","items":[{"separator":true}]}]})")));
    tui::menu::open(state, separators_only);
    ASSERT(state.mode == Mode::Bar);
    tui::menu::open_items(state, separators_only); // nothing selectable
    ASSERT(state.mode == Mode::Bar);
    tui::menu::move_item(state, separators_only, 1); // stays at the bar
    ASSERT(state.mode == Mode::Bar);

    tui::menu::open(state, model);
    ASSERT(state.mode == Mode::Bar);
    tui::menu::open_items(state, model);
    ASSERT(state.mode == Mode::Items);
    tui::menu::open_items(state, model); // wrong mode: no-op
    ASSERT(state.mode == Mode::Items);
}

TEST(menu_move_menu_leaves_a_submenu) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);
    state.item = 2;
    tui::menu::activate(state, model);
    ASSERT(state.mode == Mode::Submenu);
    tui::menu::move_menu(state, model, 1);
    ASSERT(state.mode == Mode::Items);
    ASSERT_EQ(state.menu, 1u);
}

TEST(menu_submenu_navigation_skips_separators_and_wraps) {
    const Model model = submenu_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);
    ASSERT_EQ(state.item, 0u);
    tui::menu::activate(state, model);
    ASSERT(state.mode == Mode::Submenu);
    tui::menu::move_item(state, model, 1);
    ASSERT_EQ(state.submenu, 2u); // separator skipped
    tui::menu::move_item(state, model, 1);
    ASSERT_EQ(state.submenu, 0u); // wraps
    tui::menu::move_item(state, model, -1);
    ASSERT_EQ(state.submenu, 2u);

    // Children that are only a separator have no next selection.
    state.item = 1;
    state.submenu = 0;
    tui::menu::move_item(state, model, 1);
    ASSERT_EQ(state.submenu, 0u);

    // An item with no children at all is a no-op too.
    state.item = 2;
    tui::menu::move_item(state, model, 1);
    ASSERT_EQ(state.submenu, 0u);
}

TEST(menu_activate_guards_cover_invalid_states) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    ASSERT_FALSE(tui::menu::activate(state, model).valid); // bar opens the list
    ASSERT(state.mode == Mode::Items);

    tui::menu::close(state);
    ASSERT_FALSE(tui::menu::activate(state, model).valid);

    state.mode = Mode::Items;
    state.menu = 99;
    ASSERT_FALSE(tui::menu::activate(state, model).valid);

    state.menu = 0;
    state.item = 1; // separator
    ASSERT_FALSE(tui::menu::activate(state, model).valid);

    state.item = 99;
    ASSERT_FALSE(tui::menu::activate(state, model).valid);

    state.mode = Mode::Submenu;
    state.item = 99;
    ASSERT_FALSE(tui::menu::activate(state, model).valid);

    state.item = 2; // Theme
    state.submenu = 99;
    ASSERT_FALSE(tui::menu::activate(state, model).valid);

    const Model with_separator = submenu_model();
    State submenu_state;
    tui::menu::open(submenu_state, with_separator);
    tui::menu::open_items(submenu_state, with_separator);
    submenu_state.mode = Mode::Submenu;
    submenu_state.submenu = 1; // separator between Classic and Modern
    ASSERT_FALSE(tui::menu::activate(submenu_state, with_separator).valid);

    // A nested child is a submenu, not a leaf action.
    submenu_state.item = 3; // Nested
    submenu_state.submenu = 0;
    const auto nested = tui::menu::activate(submenu_state, with_separator);
    ASSERT_FALSE(nested.valid);
}

TEST(menu_letters_cover_guards_and_submenus) {
    const Model model = submenu_model();
    State state;
    ASSERT_FALSE(tui::menu::select_letter(state, model, 'f')); // closed

    Model empty;
    state.mode = Mode::Bar;
    ASSERT_FALSE(tui::menu::select_letter(state, empty, 'f'));

    tui::menu::open(state, model);
    ASSERT_FALSE(tui::menu::select_letter(state, model, 'z')); // no menu match
    ASSERT(tui::menu::select_letter(state, model, 'f'));
    ASSERT(state.mode == Mode::Items);
    ASSERT_FALSE(tui::menu::select_letter(state, model, 'z')); // no item match

    state.menu = 99;
    ASSERT_FALSE(tui::menu::select_letter(state, model, 'f'));

    state.menu = 0;
    state.mode = Mode::Submenu;
    state.item = 0;
    state.submenu = 0;
    ASSERT(tui::menu::select_letter(state, model, 'm'));
    ASSERT_EQ(state.submenu, 2u);
    ASSERT_FALSE(tui::menu::select_letter(state, model, 'z'));

    state.item = 99;
    ASSERT_FALSE(tui::menu::select_letter(state, model, 'm'));
}

TEST(menu_bar_navigation_skips_separators_and_wraps) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    ASSERT(state.mode == Mode::Bar);
    ASSERT_EQ(state.menu, 0u);

    tui::menu::move_item(state, model, 1);
    ASSERT(state.mode == Mode::Items);
    ASSERT_EQ(state.item, 0u);
    tui::menu::move_item(state, model, 1);
    ASSERT_EQ(state.item, 2u); // separator skipped
    tui::menu::move_item(state, model, 1);
    ASSERT_EQ(state.item, 3u);
    tui::menu::move_item(state, model, 1);
    ASSERT_EQ(state.item, 0u); // wraps past the separator
}

TEST(menu_activate_returns_a_leaf_action_and_opens_a_submenu) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);
    const auto leaf = tui::menu::activate(state, model);
    ASSERT(leaf.valid);
    ASSERT_EQ(leaf.action, std::string("core.window.new"));

    tui::menu::move_item(state, model, 1);
    ASSERT_EQ(state.item, 2u);
    const auto child = tui::menu::activate(state, model);
    ASSERT_FALSE(child.valid);
    ASSERT(state.mode == Mode::Submenu);
    ASSERT_EQ(state.submenu, 0u);

    const auto classic = tui::menu::activate(state, model);
    ASSERT(classic.valid);
    ASSERT_EQ(classic.action, std::string("ui.theme.classic"));
}

TEST(menu_back_unwinds_submenu_then_bar_then_closed) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);
    state.item = 2;
    tui::menu::activate(state, model);
    ASSERT(state.mode == Mode::Submenu);
    tui::menu::back(state);
    ASSERT(state.mode == Mode::Items);
    tui::menu::back(state);
    ASSERT(state.mode == Mode::Bar);
    tui::menu::back(state);
    ASSERT(state.mode == Mode::Closed);
}

TEST(menu_letters_open_a_menu_and_select_an_item) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    ASSERT(tui::menu::select_letter(state, model, 'h'));
    ASSERT_EQ(state.menu, 1u);
    ASSERT(state.mode == Mode::Items);
    ASSERT_FALSE(tui::menu::select_letter(state, model, 'f')); // no Help item starts with f
    tui::menu::back(state);
    ASSERT(tui::menu::select_letter(state, model, 'f'));
    ASSERT_EQ(state.menu, 0u);
    ASSERT(state.mode == Mode::Items);
    ASSERT(tui::menu::select_letter(state, model, 'q'));
    ASSERT_EQ(state.item, 3u);
}

TEST(menu_layout_places_titles_and_a_bounded_dropdown) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    auto layout = tui::menu::layout(model, state, 80, 2, 23);
    ASSERT_EQ(layout.titles.size(), 2u);
    ASSERT(layout.titles[0].rect.x == 3);
    ASSERT(layout.dropdown.w == 0);

    tui::menu::open_items(state, model);
    layout = tui::menu::layout(model, state, 80, 2, 23);
    ASSERT(layout.dropdown.visible());
    ASSERT(layout.dropdown.y == 2);
    ASSERT(layout.dropdown.x >= 0);
    ASSERT(layout.dropdown.x + layout.dropdown.w <= 80);
    ASSERT(layout.dropdown.y + layout.dropdown.h <= 23);
    ASSERT_EQ(layout.items.size(), 3u); // New, Theme, Quit; separator omitted
    ASSERT(layout.items[0].rect.y == 3);
    ASSERT(layout.items[1].rect.y == 5);
}

TEST(menu_layout_opens_a_submenu_beside_the_item) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);
    state.item = 2;
    tui::menu::activate(state, model);
    const auto layout = tui::menu::layout(model, state, 80, 2, 23);
    ASSERT(layout.submenu.visible());
    ASSERT(layout.submenu.x > layout.dropdown.x);
    ASSERT(layout.submenu.x + layout.submenu.w <= 80);
    ASSERT_EQ(layout.sub_items.size(), 2u);
}

TEST(menu_hit_test_matches_the_painted_layout) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);
    const auto layout = tui::menu::layout(model, state, 80, 2, 23);

    const auto title = tui::menu::hit_test(layout, layout.titles[0].rect.x, 0);
    ASSERT(title.has_value());
    ASSERT(title->kind == tui::menu::HitKind::Title);
    ASSERT_EQ(title->menu, 0u);

    const auto item = tui::menu::hit_test(layout, layout.items[0].rect.x, layout.items[0].rect.y);
    ASSERT(item.has_value());
    ASSERT(item->kind == tui::menu::HitKind::Item);
    ASSERT_FALSE(item->submenu);

    const auto inside = tui::menu::hit_test(layout, layout.dropdown.x, layout.dropdown.y);
    ASSERT(inside.has_value());
    ASSERT(inside->kind == tui::menu::HitKind::Inside);

    ASSERT_FALSE(tui::menu::hit_test(layout, 79, 23).has_value());
}

TEST(menu_layout_closed_or_empty_is_empty) {
    const Model model = sample_model();
    State state;
    const auto closed = tui::menu::layout(model, state, 80, 2, 23);
    ASSERT(closed.titles.empty());
    ASSERT_FALSE(closed.dropdown.visible());

    Model empty;
    tui::menu::open(state, empty);
    ASSERT(state.mode == Mode::Closed);
}

TEST(menu_layout_stays_inside_a_narrow_screen) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);
    for (int width = 16; width <= 80; ++width) {
        const auto layout = tui::menu::layout(model, state, width, 2, 12);
        if (layout.dropdown.visible()) {
            ASSERT(layout.dropdown.x >= 0);
            ASSERT(layout.dropdown.x + layout.dropdown.w <= width);
            ASSERT(layout.dropdown.y + layout.dropdown.h <= 12);
        }
    }
}

TEST(menu_layout_handles_tight_vertical_space) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);

    const auto tight = tui::menu::layout(model, state, 80, 2, 4);
    ASSERT_EQ(tight.titles.size(), 2u);
    ASSERT_FALSE(tight.dropdown.visible());

    const auto flat = tui::menu::layout(model, state, 80, 5, 5);
    ASSERT(flat.titles.empty());
    ASSERT_FALSE(flat.dropdown.visible());

    const auto narrow = tui::menu::layout(model, state, 4, 2, 23);
    ASSERT(narrow.titles.empty());
    ASSERT_FALSE(narrow.dropdown.visible());
}

TEST(menu_layout_guards_a_submenu_that_cannot_fit) {
    const Model model = sample_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);

    state.mode = Mode::Submenu;
    state.item = 99;
    auto layout = tui::menu::layout(model, state, 80, 2, 23);
    ASSERT(layout.dropdown.visible());
    ASSERT_FALSE(layout.submenu.visible());

    state.item = 0; // New session has no children
    layout = tui::menu::layout(model, state, 80, 2, 23);
    ASSERT_FALSE(layout.submenu.visible());

    const Model tall = tall_submenu_model();
    State tall_state;
    tui::menu::open(tall_state, tall);
    tui::menu::open_items(tall_state, tall);
    tall_state.mode = Mode::Submenu;
    tall_state.item = 5;
    layout = tui::menu::layout(tall, tall_state, 80, 2, 10);
    ASSERT(layout.dropdown.visible());
    ASSERT_FALSE(layout.submenu.visible()); // only one row left below it
}

TEST(menu_layout_skips_a_separator_in_a_submenu) {
    const Model model = submenu_model();
    State state;
    tui::menu::open(state, model);
    tui::menu::open_items(state, model);
    ASSERT_EQ(state.item, 0u);
    tui::menu::activate(state, model);
    const auto layout = tui::menu::layout(model, state, 80, 2, 23);
    ASSERT(layout.submenu.visible());
    ASSERT_EQ(layout.sub_items.size(), 2u); // Classic, Modern; separator omitted

    const auto hit =
        tui::menu::hit_test(layout, layout.sub_items[1].rect.x, layout.sub_items[1].rect.y);
    ASSERT(hit.has_value());
    ASSERT(hit->kind == tui::menu::HitKind::Item);
    ASSERT(hit->submenu);
    ASSERT_EQ(hit->item, 2u);
}

TEST(menu_controller_opens_runs_and_unwinds) {
    bool ran = false;
    std::string action;
    tui::MenuController controller(
        [&](const std::string& a, const std::string&) {
            ran = true;
            action = a;
            return true;
        },
        [] {}, [](const std::string&) {});
    controller.load_data();
    ASSERT(controller.loaded());
    ASSERT_FALSE(controller.is_open());

    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(controller.is_open());
    ASSERT(controller.handle_key(tui::keys::kRight));
    ASSERT_EQ(controller.state().menu, 1u);
    ASSERT(controller.handle_key(tui::keys::kLeft));
    ASSERT_EQ(controller.state().menu, 0u);
    ASSERT(controller.handle_key(tui::keys::kDown));
    ASSERT(controller.state().mode == Mode::Items);
    ASSERT(controller.handle_key('\r'));
    ASSERT(ran);
    ASSERT_EQ(action, std::string("core.window.new"));
    ASSERT_FALSE(controller.is_open());

    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(controller.is_open());
    ASSERT(controller.handle_key(tui::keys::kF10)); // toggle from the open state
    ASSERT_FALSE(controller.is_open());

    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(controller.handle_key('\r')); // Bar: opens the list, no action yet
    ASSERT(controller.is_open());
    ASSERT(controller.state().mode == Mode::Items);
    ASSERT(controller.handle_key(tui::keys::kUp));
    controller.close();
    ASSERT_FALSE(controller.is_open());

    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(controller.is_open());
    ASSERT(controller.handle_key(tui::keys::kDown));
    ASSERT(controller.handle_key(27));
    ASSERT(controller.handle_key(27));
    ASSERT_FALSE(controller.is_open());
}

TEST(menu_controller_without_data_reports_unavailable) {
    std::string message;
    int redraws = 0;
    tui::MenuController controller([](const std::string&, const std::string&) { return true; },
                                   [&] { ++redraws; }, [&](const std::string& m) { message = m; });
    ASSERT_FALSE(controller.loaded());
    ASSERT_FALSE(controller.is_open());
    ASSERT_FALSE(controller.handle_key('a'));
    ASSERT_FALSE(controller.handle_key(tui::keys::kMouse));
    controller.close(); // closed: no redraw

    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(message.find("menu unavailable") != std::string::npos);
    ASSERT_FALSE(controller.is_open());
    ASSERT_EQ(redraws, 0);
    ASSERT_TRUE(controller.hint().empty());
}

TEST(menu_controller_handles_hits_and_letters) {
    std::string ran;
    std::string status;
    int redraws = 0;
    tui::MenuController controller(
        [&](const std::string& action, const std::string&) {
            ran = action;
            return true;
        },
        [&] { ++redraws; }, [&](const std::string& m) { status = m; });
    controller.load_data();
    ASSERT(controller.loaded());

    ASSERT(controller.handle_hit({tui::menu::HitKind::Inside, 0, 0, false}));
    ASSERT_FALSE(controller.is_open());

    ASSERT(controller.handle_hit({tui::menu::HitKind::Title, 4, 0, false}));
    ASSERT(controller.is_open());
    ASSERT_EQ(controller.state().menu, 4u);
    ASSERT(controller.state().mode == Mode::Items);
    ASSERT(controller.handle_key(tui::keys::kDown));
    ASSERT(controller.handle_key('\r'));
    ASSERT_EQ(ran, std::string("core.config.get.config.paths"));
    ASSERT_FALSE(controller.is_open());

    // A letter selects a menu once the bar is open; unmapped keys are consumed.
    ASSERT_FALSE(controller.handle_key('f'));
    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(controller.handle_key('h'));
    ASSERT(controller.is_open());
    ASSERT_EQ(controller.state().menu, 4u);
    ASSERT(controller.handle_key('z'));
    ASSERT(controller.handle_key(1)); // non-printable: consumed after navigation
    ASSERT(controller.is_open());
    ASSERT(controller.handle_key(27));
    ASSERT(controller.handle_key(27));

    // A hit on an item row activates the action.
    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(controller.handle_key('f'));
    ASSERT(controller.handle_hit({tui::menu::HitKind::Item, 0, 0, false}));
    ASSERT_FALSE(controller.is_open());
    ASSERT_EQ(ran, std::string("core.window.new"));

    // The submenu flag takes the submenu branch on the same row.
    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(controller.handle_key('f'));
    ASSERT(controller.handle_hit({tui::menu::HitKind::Item, 0, 0, true}));
    ASSERT_FALSE(controller.is_open());
}

TEST(menu_controller_reports_an_unavailable_action) {
    std::string status;
    tui::MenuController controller([](const std::string&, const std::string&) { return false; },
                                   [] {}, [&](const std::string& m) { status = m; });
    controller.load_data();
    ASSERT(controller.loaded());
    ASSERT(controller.handle_key(tui::keys::kF10));
    ASSERT(controller.handle_key(tui::keys::kDown));
    ASSERT(controller.handle_key('\r'));
    ASSERT(status.find("menu action unavailable") != std::string::npos);
    ASSERT(status.find("core.window.new") != std::string::npos);
}

TEST(menu_controller_hint_waits_for_idle_and_rearms) {
    using namespace std::chrono_literals;
    tui::MenuController controller([](const std::string&, const std::string&) { return true; },
                                   [] {}, [](const std::string&) {});
    controller.load_data();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT(controller.update_hint(t0, false).empty());
    controller.note_input(t0);
    ASSERT(controller.update_hint(t0 + 5s, true).empty()); // idle starts at the key
    const std::string shown = controller.update_hint(t0 + 10s, true);
    ASSERT_FALSE(shown.empty());
    ASSERT_EQ(controller.hint(), shown);
    ASSERT(controller.update_hint(t0 + 11s, false).empty());
    ASSERT(controller.hint().empty());
    ASSERT(controller.update_hint(t0 + 16s, true).empty()); // idle restarts on re-arm
    ASSERT_FALSE(controller.update_hint(t0 + 21s, true).empty());
}

TEST(bundled_data_candidates_ignore_user_data_roots) {
    const char* old_ws = std::getenv("AMBER_WORKSPACE");
    const char* old_xdg = std::getenv("XDG_DATA_HOME");
    const std::string saved_ws = old_ws ? old_ws : "";
    const std::string saved_xdg = old_xdg ? old_xdg : "";
    setenv("AMBER_WORKSPACE", "/tmp/amber-bundled-ws", 1);
    setenv("XDG_DATA_HOME", "/tmp/amber-bundled-xdg", 1);

    const auto candidates = agent::bundled_data_candidates("menus.json", "/opt/amber/bin/amber");
    for (const auto& candidate : candidates) {
        ASSERT(candidate.find("/tmp/amber-bundled-ws") == std::string::npos);
        ASSERT(candidate.find("/tmp/amber-bundled-xdg") == std::string::npos);
    }

    if (old_ws)
        setenv("AMBER_WORKSPACE", saved_ws.c_str(), 1);
    else
        unsetenv("AMBER_WORKSPACE");
    if (old_xdg)
        setenv("XDG_DATA_HOME", saved_xdg.c_str(), 1);
    else
        unsetenv("XDG_DATA_HOME");
}

TEST(menu_actions_resolve_against_the_command_tree) {
    std::ifstream menus_file("menus.json");
    ASSERT(menus_file.is_open());
    nlohmann::json menus_json;
    menus_file >> menus_json;
    Model model;
    ASSERT(model.parse(menus_json));

    std::ifstream completions_file("completions.json");
    ASSERT(completions_file.is_open());
    nlohmann::json completions_json;
    completions_file >> completions_json;

    std::set<std::string> known;
    collect_actions(completions_json, known);
    for (const char* ui :
         {"ui.tab.next", "ui.tab.prev", "ui.view.reasoning", "ui.mouse.toggle", "ui.scroll.toggle",
          "ui.input.delete_word", "ui.escape", "ui.cancel_or_quit"})
        known.insert(ui);

    std::function<void(const std::vector<Item>&)> check = [&](const std::vector<Item>& items) {
        for (const auto& item : items) {
            if (!item.separator && !item.action.empty())
                ASSERT(known.count(item.action) != 0);
            check(item.children);
        }
    };
    for (const auto& menu : model.menus)
        check(menu.items);
}

// Keybindings are data too, and they obey the same rule: every action either
// exists in the command tree or is an internal ui.* handler. A plugin binding
// is merged at runtime (checked beside the plugin that contributes it), so the
// shipped table must not name one — that would be an action with no handler on
// a host that disabled the plugin.
TEST(keybindings_resolve_and_have_no_collisions) {
    std::ifstream bindings_file("keybindings.json");
    ASSERT(bindings_file.is_open());
    nlohmann::json bindings_json;
    bindings_file >> bindings_json;

    std::ifstream completions_file("completions.json");
    ASSERT(completions_file.is_open());
    nlohmann::json completions_json;
    completions_file >> completions_json;

    std::set<std::string> known;
    collect_actions(completions_json, known);
    for (const char* ui :
         {"ui.tab.next", "ui.tab.prev", "ui.view.reasoning", "ui.mouse.toggle", "ui.scroll.toggle",
          "ui.input.delete_word", "ui.escape", "ui.cancel_or_quit"})
        known.insert(ui);

    const tui::KeyBinder binder(bindings_json);
    ASSERT_EQ(binder.collisions(), 0u);
    ASSERT(!binder.bindings().empty());

    std::set<std::string> triples;
    for (const auto& binding : binder.bindings()) {
        ASSERT(known.count(binding.action) != 0);
        ASSERT(binding.scope == "global" || binding.scope == "input" || binding.scope == "menu" ||
               binding.scope == "dialog" || binding.scope == "list");
        ASSERT(binding.when == "always" || binding.when == "drawer" || binding.when == "busy" ||
               binding.when == "idle");
        ASSERT(triples.insert(binding.key + "|" + binding.scope + "|" + binding.when).second);
    }
}

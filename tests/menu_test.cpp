// The menu tree, its selection state and the painted rectangle model. Pure L1:
// the renderer and the mouse handler both use the same layout.

#include "tui/menu_controller.h"
#include "tui/menu_model.h"
#include "tui/menu_view.h"
#include "tui/keys.h"
#include "test_util.h"

#include <nlohmann/json.hpp>

#include <fstream>
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
    ASSERT(controller.handle_key(tui::keys::kDown));
    ASSERT(controller.handle_key(27));
    ASSERT(controller.handle_key(27));
    ASSERT_FALSE(controller.is_open());
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
         {"ui.tab.next", "ui.tab.prev", "ui.view.reasoning", "ui.mouse.toggle", "ui.scroll.toggle"})
        known.insert(ui);

    for (const auto& menu : model.menus) {
        for (const auto& item : menu.items) {
            if (item.separator || item.action.empty())
                continue;
            ASSERT(known.count(item.action) != 0);
        }
    }
}

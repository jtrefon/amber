#include "tui/menu_model.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>

namespace tui::menu {

namespace {

std::optional<std::size_t> first_selectable(const std::vector<Item>& items, std::size_t from = 0) {
    for (std::size_t i = from; i < items.size(); ++i)
        if (!items[i].separator)
            return i;
    return std::nullopt;
}

std::optional<std::size_t> step_selectable(const std::vector<Item>& items, std::size_t from,
                                           int delta) {
    if (items.empty())
        return std::nullopt;
    const std::size_t n = items.size();
    std::size_t i = from;
    for (std::size_t tries = 0; tries < n; ++tries) {
        i = (i + n + (delta < 0 ? n - 1 : 1)) % n;
        if (!items[i].separator)
            return i;
    }
    return std::nullopt;
}

char lower_ascii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::optional<std::size_t> letter_match(const std::vector<Item>& items, char wanted) {
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!items[i].separator && !items[i].label.empty() &&
            lower_ascii(items[i].label.front()) == wanted)
            return i;
    return std::nullopt;
}

bool parse_item(const nlohmann::json& json, Item& item) {
    if (!json.is_object())
        return false;
    if (json.value("separator", false)) {
        item.separator = true;
        return true;
    }
    item.label = json.value("label", std::string{});
    if (item.label.empty())
        return false;
    item.action = json.value("action", std::string{});
    item.arg = json.value("arg", std::string{});
    if (json.contains("children")) {
        if (!json["children"].is_array())
            return false;
        for (const auto& child : json["children"]) {
            Item parsed;
            if (!parse_item(child, parsed))
                return false;
            item.children.push_back(std::move(parsed));
        }
    }
    return !item.action.empty() || !item.children.empty();
}

} // namespace

bool Model::parse(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("menus") || !json["menus"].is_array())
        return false;
    std::vector<Menu> parsed_menu;
    for (const auto& jm : json["menus"]) {
        if (!jm.is_object())
            return false;
        Menu menu;
        menu.label = jm.value("label", std::string{});
        if (menu.label.empty() || !jm.contains("items") || !jm["items"].is_array())
            return false;
        for (const auto& ji : jm["items"]) {
            Item item;
            if (!parse_item(ji, item))
                return false;
            menu.items.push_back(std::move(item));
        }
        if (menu.items.empty())
            return false;
        parsed_menu.push_back(std::move(menu));
    }
    if (parsed_menu.empty())
        return false;
    menus = std::move(parsed_menu);
    return true;
}

bool Model::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return false;
    try {
        nlohmann::json json;
        file >> json;
        return parse(json);
    } catch (const std::exception&) {
        return false;
    }
}

void open(State& state, const Model& model) {
    if (model.menus.empty()) {
        close(state);
        return;
    }
    state.mode = Mode::Bar;
    state.menu = 0;
    state.item = 0;
    state.submenu = 0;
}

void close(State& state) {
    state.mode = Mode::Closed;
    state.menu = 0;
    state.item = 0;
    state.submenu = 0;
}

void move_menu(State& state, const Model& model, int delta) {
    if (state.mode == Mode::Closed || model.menus.empty())
        return;
    const std::size_t n = model.menus.size();
    state.menu = (state.menu + n + (delta < 0 ? n - 1 : 1)) % n;
    const auto first = first_selectable(model.menus[state.menu].items);
    state.item = first.value_or(0);
    state.submenu = 0;
    if (state.mode == Mode::Submenu)
        state.mode = Mode::Items;
}

void open_items(State& state, const Model& model) {
    if (state.mode != Mode::Bar || state.menu >= model.menus.size())
        return;
    const auto first = first_selectable(model.menus[state.menu].items);
    if (!first)
        return;
    state.mode = Mode::Items;
    state.item = *first;
}

void move_item(State& state, const Model& model, int delta) {
    if (state.mode == Mode::Bar) {
        open_items(state, model);
        return;
    }
    if (state.mode == Mode::Closed || state.menu >= model.menus.size())
        return;
    if (state.mode == Mode::Items) {
        const auto next = step_selectable(model.menus[state.menu].items, state.item, delta);
        if (next)
            state.item = *next;
        return;
    }
    const auto& items = model.menus[state.menu].items;
    if (state.item >= items.size())
        return;
    const auto& children = items[state.item].children;
    const auto next = step_selectable(children, state.submenu, delta);
    if (next)
        state.submenu = *next;
}

Selection activate(State& state, const Model& model) {
    Selection selection;
    if (state.mode == Mode::Bar) {
        open_items(state, model);
        return selection;
    }
    if (state.mode == Mode::Closed || state.menu >= model.menus.size())
        return selection;
    const auto& items = model.menus[state.menu].items;
    if (state.mode == Mode::Items) {
        if (state.item >= items.size() || items[state.item].separator)
            return selection;
        const Item& item = items[state.item];
        if (!item.children.empty()) {
            const auto first = first_selectable(item.children);
            if (first) {
                state.mode = Mode::Submenu;
                state.submenu = *first;
            }
            return selection;
        }
        selection.action = item.action;
        selection.arg = item.arg;
        selection.valid = !selection.action.empty();
        return selection;
    }
    if (state.item >= items.size())
        return selection;
    const auto& children = items[state.item].children;
    if (state.submenu >= children.size() || children[state.submenu].separator)
        return selection;
    const Item& item = children[state.submenu];
    selection.action = item.action;
    selection.arg = item.arg;
    selection.valid = !selection.action.empty() && item.children.empty();
    return selection;
}

void back(State& state) {
    switch (state.mode) {
    case Mode::Submenu:
        state.mode = Mode::Items;
        state.submenu = 0;
        break;
    case Mode::Items:
        state.mode = Mode::Bar;
        state.item = 0;
        break;
    case Mode::Bar:
    case Mode::Closed:
        close(state);
        break;
    }
}

bool select_letter(State& state, const Model& model, char letter) {
    if (state.mode == Mode::Closed || model.menus.empty())
        return false;
    const char wanted = lower_ascii(letter);
    if (state.mode == Mode::Bar) {
        for (std::size_t i = 0; i < model.menus.size(); ++i) {
            if (!model.menus[i].label.empty() &&
                lower_ascii(model.menus[i].label.front()) == wanted) {
                state.menu = i;
                open_items(state, model);
                return true;
            }
        }
        return false;
    }
    if (state.menu >= model.menus.size())
        return false;
    const auto& items = model.menus[state.menu].items;
    if (state.mode == Mode::Items) {
        const auto index = letter_match(items, wanted);
        if (!index)
            return false;
        state.item = *index;
        return true;
    }
    if (state.item >= items.size())
        return false;
    const auto index = letter_match(items[state.item].children, wanted);
    if (!index)
        return false;
    state.submenu = *index;
    return true;
}

} // namespace tui::menu

#ifndef AMBER_TUI_MENU_MODEL_H
#define AMBER_TUI_MENU_MODEL_H

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// L1 (pure): the menu tree, its selection state, and the transitions the input
// layer drives. No ncurses, no action execution.
namespace tui::menu {

struct Item {
    std::string label;
    std::string action;
    std::string arg;
    std::vector<Item> children;
    bool separator = false;
};

struct Menu {
    std::string label;
    std::vector<Item> items;
};

struct Model {
    std::vector<Menu> menus;
    bool parse(const nlohmann::json& json);
    bool load(const std::string& path);
};

enum class Mode {
    Closed,
    Bar,
    Items,
    Submenu,
};

struct State {
    Mode mode = Mode::Closed;
    std::size_t menu = 0;
    std::size_t item = 0;
    std::size_t submenu = 0;
};

struct Selection {
    std::string action;
    std::string arg;
    bool valid = false;
};

void open(State& state, const Model& model);
void close(State& state);
void move_menu(State& state, const Model& model, int delta);
void move_item(State& state, const Model& model, int delta);
void open_items(State& state, const Model& model);
Selection activate(State& state, const Model& model);
void back(State& state);
bool select_letter(State& state, const Model& model, char letter);

} // namespace tui::menu

#endif // AMBER_TUI_MENU_MODEL_H

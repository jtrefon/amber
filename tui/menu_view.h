#ifndef AMBER_TUI_MENU_VIEW_H
#define AMBER_TUI_MENU_VIEW_H

#include <cstddef>
#include <optional>
#include <vector>

#include "tui/menu_model.h"

// L1 (pure): where the painted menu titles and dropdowns sit. The renderer paints
// this; the mouse handler hit-tests the same rectangles.
namespace tui::menu {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool contains(int px, int py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    bool visible() const noexcept { return w > 0 && h > 0; }
};

struct TitleHit {
    std::size_t menu = 0;
    Rect rect;
};

struct ItemHit {
    std::size_t menu = 0;
    std::size_t item = 0;
    Rect rect;
    bool submenu = false;
};

struct Layout {
    std::vector<TitleHit> titles;
    Rect dropdown;
    std::vector<ItemHit> items;
    Rect submenu;
    std::vector<ItemHit> sub_items;
};

// `top` is the first row available below the rail; `bottom` is exclusive (the
// status row). The dropdown is clamped inside both.
Layout layout(const Model& model, const State& state, int width, int top, int bottom);

enum class HitKind {
    Title,
    Item,
    Inside,
};

struct Hit {
    HitKind kind = HitKind::Inside;
    std::size_t menu = 0;
    std::size_t item = 0;
    bool submenu = false;
};

// The topmost thing at a screen cell. Null means the click is outside every
// menu surface, which closes the menu.
std::optional<Hit> hit_test(const Layout& layout, int x, int y);

} // namespace tui::menu

#endif // AMBER_TUI_MENU_VIEW_H

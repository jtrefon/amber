#include "tui/menu_view.h"

#include "tui/textutil.h"
#include "tui/top_bar.h"

#include <algorithm>

namespace tui::menu {

namespace {

int title_width(const Menu& menu) {
    return text::display_cols(menu.label) + 2;
}

int items_width(const std::vector<Item>& items) {
    int width = 0;
    for (const auto& item : items)
        if (!item.separator)
            width = std::max(width, text::display_cols(item.label));
    return width + 6;
}

int box_width(int natural, int screen) {
    return std::min(std::max(natural, 16), std::max(1, screen - 2));
}

struct Box {
    Rect rect;
    std::vector<ItemHit> items;
};

std::vector<TitleHit> titles_for(const Model& model, int width) {
    std::vector<TitleHit> titles;
    int x = top_bar::kAppMarkWidth + top_bar::kAppGap;
    for (std::size_t i = 0; i < model.menus.size(); ++i) {
        const int w = title_width(model.menus[i]);
        if (x + w > width)
            break;
        titles.push_back({i, {x, 0, w, 1}});
        x += w;
    }
    return titles;
}

Box dropdown_for(const Model& model, const State& state, int width, int top, int bottom,
                 const std::vector<TitleHit>& titles) {
    Box box;
    const auto& items = model.menus[state.menu].items;
    const int w = box_width(items_width(items), width);
    const int anchor = state.menu < titles.size() ? titles[state.menu].rect.x : 0;
    const int x = std::min(anchor, std::max(0, width - w));
    const int h = std::min(static_cast<int>(items.size()) + 2, bottom - top);
    if (h < 3)
        return box;
    box.rect = {x, top, w, h};
    const int rows = h - 2;
    for (int i = 0; i < rows && i < static_cast<int>(items.size()); ++i) {
        if (items[static_cast<std::size_t>(i)].separator)
            continue;
        box.items.push_back(
            {state.menu, static_cast<std::size_t>(i), {x + 2, top + 1 + i, w - 4, 1}, false});
    }
    return box;
}

Box submenu_for(const Model& model, const State& state, int width, int bottom, const Box& parent) {
    Box box;
    const auto& items = model.menus[state.menu].items;
    if (state.item >= items.size() || state.item >= static_cast<std::size_t>(parent.rect.h - 2))
        return box;
    const auto& children = items[state.item].children;
    if (children.empty())
        return box;
    const int w = box_width(items_width(children), width);
    const int x = std::max(0, std::min(parent.rect.x + parent.rect.w - 1, width - w));
    const int y = std::min(parent.rect.y + 1 + static_cast<int>(state.item), bottom - 1);
    const int h = std::min(static_cast<int>(children.size()) + 2, bottom - y);
    if (h < 3)
        return box;
    box.rect = {x, y, w, h};
    const int rows = h - 2;
    for (int i = 0; i < rows && i < static_cast<int>(children.size()); ++i) {
        if (children[static_cast<std::size_t>(i)].separator)
            continue;
        box.items.push_back(
            {state.menu, static_cast<std::size_t>(i), {x + 2, y + 1 + i, w - 4, 1}, true});
    }
    return box;
}

} // namespace

std::optional<Hit> hit_test(const Layout& layout, int x, int y) {
    for (const auto& item : layout.sub_items)
        if (item.rect.contains(x, y))
            return Hit{HitKind::Item, item.menu, item.item, true};
    for (const auto& item : layout.items)
        if (item.rect.contains(x, y))
            return Hit{HitKind::Item, item.menu, item.item, false};
    for (const auto& title : layout.titles)
        if (title.rect.contains(x, y))
            return Hit{HitKind::Title, title.menu, 0, false};
    if (layout.dropdown.contains(x, y) || layout.submenu.contains(x, y))
        return Hit{HitKind::Inside, 0, 0, false};
    return std::nullopt;
}

Layout layout(const Model& model, const State& state, int width, int top, int bottom) {
    Layout out;
    if (state.mode == Mode::Closed || model.menus.empty() || width <= 0 || top >= bottom)
        return out;
    out.titles = titles_for(model, width);
    if (out.titles.empty() || state.mode == Mode::Bar || state.menu >= model.menus.size())
        return out;

    Box dropdown = dropdown_for(model, state, width, top, bottom, out.titles);
    out.dropdown = dropdown.rect;
    out.items = std::move(dropdown.items);
    if (state.mode != Mode::Submenu)
        return out;

    Box submenu = submenu_for(model, state, width, bottom, dropdown);
    out.submenu = submenu.rect;
    out.sub_items = std::move(submenu.items);
    return out;
}

} // namespace tui::menu

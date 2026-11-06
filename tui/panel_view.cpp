#include "panel_view.h"

#include "tui/panel_view_state.h"
#include "tui/screen_view.h"
#include "tui/terminal.h"
#include "tui/widgets.h"

#include "agent/extensions.h"

namespace tui {

namespace {

std::vector<std::string> panel_ids(const std::vector<agent::PanelSpec>& specs) {
    std::vector<std::string> ids;
    ids.reserve(specs.size());
    for (const auto& s : specs)
        ids.push_back(s.id);
    return ids;
}

std::string footer_text(int panel_count) {
    std::string text;
    for (const auto& [key, action] : panel_view_state::footer(panel_count)) {
        if (!text.empty())
            text += "   ";
        text += '[';
        text += key;
        text += "] ";
        text += action;
    }
    return text;
}

} // namespace

namespace {

ScreenView panel_screen_view(const std::vector<agent::PanelSpec>& specs, int index,
                             panel_view_state::Scroll& scroll) {
    const agent::PanelSpec& spec = specs[static_cast<std::size_t>(index)];
    const Size screen = Terminal{}.size();
    scroll.visible = std::max(1, screen.h - 3);
    const int width = std::max(10, screen.w - 4);
    const auto lines = spec.lines ? spec.lines(width) : std::vector<std::string>{};
    scroll.total = static_cast<int>(lines.size());
    scroll.clamp();

    ScreenView view;
    view.title = spec.title.empty() ? spec.id : spec.title;
    view.footer = footer_text(static_cast<int>(specs.size()));
    for (const auto& row : panel_view_state::visible_rows(lines, width, scroll))
        view.rows.push_back({row, false, false});
    const auto arrows = panel_view_state::arrows(scroll);
    view.scroll_up = arrows.up;
    view.scroll_down = arrows.down;
    return view;
}

} // namespace

std::string panel_view(const agent::PanelRegistry& panels, const std::string& start_id) {
    auto specs = panels.all();
    if (specs.empty())
        return {};

    int index = panel_view_state::start_index(panel_ids(specs), start_id);
    const int panel_count = static_cast<int>(specs.size());
    panel_view_state::Scroll scroll;
    ScreenView view = panel_screen_view(specs, index, scroll);

    ScreenHost host;
    host.run(
        view,
        [&](int ch) {
            const agent::PanelSpec& spec = specs[static_cast<std::size_t>(index)];
            if (spec.on_key && spec.on_key(ch))
                return false;
            if (ch == 27 || ch == 'q' || ch == 'Q')
                return true;
            if (ch == '\t') {
                if (panel_count > 1)
                    index = (index + 1) % panel_count;
                scroll.top = 0;
            } else {
                panel_view_state::apply_key(ch, scroll);
            }
            view = panel_screen_view(specs, index, scroll);
            return false;
        },
        250);
    return specs[static_cast<std::size_t>(index)].id;
}

} // namespace tui

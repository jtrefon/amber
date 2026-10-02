#include "panel_view.h"

#include "tui/dialog.h"
#include "tui/panel_view_state.h"
#include "tui/widgets.h"

#include <ncurses.h>

#include "agent/extensions.h"

namespace tui {

namespace {

// Draw the panel body and its scroll arrows.
void draw_panel(WINDOW* body, const agent::PanelSpec& spec, const panel_view_state::Geometry& g,
                panel_view_state::Scroll& scroll) {
    const auto lines = spec.lines ? spec.lines(g.body_w) : std::vector<std::string>{};
    scroll.total = static_cast<int>(lines.size());
    scroll.clamp();
    const std::vector<std::string> rows = panel_view_state::visible_rows(lines, g.body_w, scroll);
    const panel_view_state::Arrows ar = panel_view_state::arrows(scroll);
    werase(body);
    for (std::size_t i = 0; i < rows.size(); ++i)
        mvwaddstr(body, static_cast<int>(i), 0, rows[i].c_str());
    if (ar.up)
        mvwaddch(body, 0, g.body_w - 1, ACS_UARROW);
    if (ar.down)
        mvwaddch(body, scroll.visible - 1, g.body_w - 1, ACS_DARROW);
    wrefresh(body);
    update_panels();
    doupdate();
}

// Esc/q close; Tab cycles when there is more than one panel; anything else
// scrolls.
bool handle_panel_key(int ch, int panel_count, int& index, panel_view_state::Scroll& scroll) {
    switch (ch) {
    case 27: // Esc
    case 'q':
    case 'Q':
        return true;
    case '\t':
        if (panel_count > 1)
            index = (index + 1) % panel_count;
        return false;
    default:
        panel_view_state::apply_key(ch, scroll);
        return false;
    }
}

std::vector<std::string> panel_ids(const std::vector<agent::PanelSpec>& specs) {
    std::vector<std::string> ids;
    ids.reserve(specs.size());
    for (const auto& s : specs)
        ids.push_back(s.id);
    return ids;
}

// The footer depends only on how many panels there are, so it is built once
// instead of on every keypress.
std::vector<FooterKey> footer_keys(int panel_count) {
    std::vector<FooterKey> footer;
    for (const auto& f : panel_view_state::footer(panel_count))
        footer.push_back({f.first, f.second});
    return footer;
}

} // namespace

std::string panel_view(const agent::PanelRegistry& panels, const std::string& start_id) {
    auto specs = panels.all();
    if (specs.empty())
        return {};

    int index = panel_view_state::start_index(panel_ids(specs), start_id);

    ModalScope scope;
    curs_set(0);

    int sh = 0, sw = 0;
    getmaxyx(stdscr, sh, sw);
    const panel_view_state::Geometry g = panel_view_state::geometry(sh, sw);

    // The frame is drawn per panel (the title changes when cycling), so the
    // dialog is constructed inside the loop. `specs` is a local copy and never
    // changes here, so the loop only has to watch for the user closing it.
    const int panel_count = static_cast<int>(specs.size());
    const std::vector<FooterKey> footer = footer_keys(panel_count);

    bool done = false;
    while (!done) {
        const agent::PanelSpec& spec = specs[static_cast<std::size_t>(index)];

        Dialog dlg(g.dh, g.dw, spec.title.empty() ? spec.id : spec.title);
        dlg.set_footer(footer);
        WINDOW* content = dlg.win();
        WINDOW* body = derwin(content, g.body_h, g.body_w, 2, 2);
        panel_view_state::Scroll scroll;
        scroll.visible = g.body_h;
        draw_panel(body, spec, g, scroll);

        const int ch = wgetch(content);
        // A panel gets first refusal on every key.
        if (spec.on_key && spec.on_key(ch))
            continue;
        if (handle_panel_key(ch, panel_count, index, scroll))
            done = true;

        // The dialog dies with this iteration; the next one redraws cleanly.
        delwin(body);
    }
    return specs[static_cast<std::size_t>(index)].id;
}

} // namespace tui

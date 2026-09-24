#include "panel_view.h"

#include "tui/dialog.h"
#include "tui/panel_view_state.h"
#include "tui/widgets.h"

#include <ncurses.h>

#include "agent/extensions.h"

namespace tui {

std::string panel_view(const agent::PanelRegistry& panels, const std::string& start_id) {
    auto specs = panels.all();
    if (specs.empty())
        return {};

    std::vector<std::string> ids;
    ids.reserve(specs.size());
    for (const auto& s : specs)
        ids.push_back(s.id);
    int index = panel_view_state::start_index(ids, start_id);

    ModalScope scope;
    curs_set(0);

    int sh = 0, sw = 0;
    getmaxyx(stdscr, sh, sw);
    const panel_view_state::Geometry g = panel_view_state::geometry(sh, sw);

    // The frame is drawn per panel (the title changes when cycling), so the
    // dialog is constructed inside the loop. `specs` is a local copy and never
    // changes here, so the loop only has to watch for the user closing it. The
    // footer depends only on how many panels there are, so it is built once
    // instead of on every keypress.
    std::vector<FooterKey> footer;
    for (const auto& f : panel_view_state::footer(static_cast<int>(specs.size())))
        footer.push_back({f.first, f.second});

    bool done = false;
    while (!done) {
        const agent::PanelSpec& spec = specs[static_cast<std::size_t>(index)];

        Dialog dlg(g.dh, g.dw, spec.title.empty() ? spec.id : spec.title);
        dlg.set_footer(footer);
        WINDOW* content = dlg.win();
        WINDOW* body = derwin(content, g.body_h, g.body_w, 2, 2);
        panel_view_state::Scroll scroll;
        scroll.visible = g.body_h;

        const auto lines = spec.lines ? spec.lines(g.body_w) : std::vector<std::string>{};
        scroll.total = static_cast<int>(lines.size());
        scroll.clamp();
        const std::vector<std::string> rows =
            panel_view_state::visible_rows(lines, g.body_w, scroll);
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

        int ch = wgetch(content);
        // A panel gets first refusal on every key.
        if (spec.on_key && spec.on_key(ch))
            continue;

        switch (ch) {
        case 27: // Esc
        case 'q':
        case 'Q':
            done = true;
            break;
        case '\t':
            if (specs.size() > 1)
                index = (index + 1) % static_cast<int>(specs.size());
            break;
        default:
            panel_view_state::apply_key(ch, scroll);
            break;
        }

        // The dialog dies with this iteration; the next one redraws cleanly.
        delwin(body);
    }
    return specs[static_cast<std::size_t>(index)].id;
}

} // namespace tui

#include "panel_view.h"

#include "tui/dialog.h"
#include "tui/widgets.h"

#include <algorithm>
#include <ncurses.h>

#include "agent/extensions.h"

namespace tui {

namespace {

struct Scroll {
    int top = 0;
    int total = 0;
    int visible = 1;
    void clamp() {
        const int max_top = std::max(0, total - visible);
        top = std::max(0, std::min(top, max_top));
    }
};

void draw_lines(WINDOW* w, int width, const std::vector<std::string>& lines, Scroll& scroll) {
    scroll.total = static_cast<int>(lines.size());
    scroll.clamp();
    werase(w);
    for (int i = 0; i < scroll.visible; ++i) {
        const int idx = scroll.top + i;
        if (idx >= scroll.total)
            break;
        std::string text = lines[idx];
        if (static_cast<int>(text.size()) > width)
            text.resize(width);
        mvwaddstr(w, i, 0, text.c_str());
    }
    if (scroll.top > 0)
        mvwaddch(w, 0, width - 1, ACS_UARROW);
    if (scroll.top + scroll.visible < scroll.total)
        mvwaddch(w, scroll.visible - 1, width - 1, ACS_DARROW);
    wrefresh(w);
}

} // namespace

std::string panel_view(const agent::PanelRegistry& panels, const std::string& start_id) {
    auto specs = panels.all();
    if (specs.empty())
        return {};

    // Start on the requested panel, else the first one (the registry
    // guarantees that is the console).
    int index = 0;
    for (std::size_t i = 0; i < specs.size(); ++i)
        if (specs[i].id == start_id)
            index = static_cast<int>(i);

    ModalScope scope;
    curs_set(0);

    int sh = 0, sw = 0;
    getmaxyx(stdscr, sh, sw);
    const int dh = std::max(6, sh - 4);
    const int dw = std::max(20, sw - 6);

    // The frame is drawn per panel (the title changes when cycling), so the
    // dialog is constructed inside the loop. `specs` is a local copy and never
    // changes here, so the loop only has to watch for the user closing it.
    // The footer depends only on how many panels there are, so it is built once
    // instead of on every keypress.
    std::vector<FooterKey> footer;
    if (specs.size() > 1)
        footer.push_back({"Tab", "next panel"});
    footer.push_back({"Up/Down", "scroll"});
    footer.push_back({"Esc/q", "close"});

    bool done = false;
    while (!done) {
        const agent::PanelSpec& spec = specs[static_cast<std::size_t>(index)];

        Dialog dlg(dh, dw, spec.title.empty() ? spec.id : spec.title);
        dlg.set_footer(footer);
        WINDOW* content = dlg.win();
        WINDOW* body = derwin(content, dh - 4, dw - 4, 2, 2);
        keypad(content, TRUE);
        Scroll scroll;
        scroll.visible = dh - 4;

        const auto lines = spec.lines ? spec.lines(dw - 4) : std::vector<std::string>{};
        draw_lines(body, dw - 4, lines, scroll);
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
        case KEY_UP:
            --scroll.top;
            break;
        case KEY_DOWN:
            ++scroll.top;
            break;
        case KEY_PPAGE:
            scroll.top -= scroll.visible;
            break;
        case KEY_NPAGE:
            scroll.top += scroll.visible;
            break;
        case KEY_HOME:
            scroll.top = 0;
            break;
        case KEY_END:
            scroll.top = scroll.total;
            break;
        default:
            break;
        }

        // The dialog dies with this iteration; the next one redraws cleanly.
        delwin(body);
    }
    return specs[static_cast<std::size_t>(index)].id;
}

} // namespace tui

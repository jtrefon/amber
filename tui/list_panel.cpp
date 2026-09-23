
#include "tui/list_panel.h"

namespace tui {

ListPanel::ListPanel(const std::string& title, const std::vector<std::string>& items)
    : Panel(std::min(static_cast<int>(items.size()) + 2, 20),
            std::max(static_cast<int>(title.size()) + 8, 50), title,
            {{"Up/Down", "navigate"}, {"Enter", "select"}, {"/", "filter"}, {"Esc", "cancel"}}),
      state_(items) {}

ListPanel::ListPanel(const std::string& title, const std::vector<std::string>& items,
                     std::vector<FooterKey> footer)
    : Panel(std::min(static_cast<int>(items.size()) + 2, 20),
            std::max(static_cast<int>(title.size()) + 8, 50), title, std::move(footer)),
      state_(items) {}

int ListPanel::run() {
    // Blocking read; restore the main loop's tick timeout on exit. Without
    // this, the list auto-closes after the 50 ms tick when no approval
    // dialog has switched stdscr to blocking yet.
    BlockingInputGuard input_guard;
    timeout(-1);
    draw_items();
    show();
    run_input_loop();
    hide();
    state_.remap_selection();
    return state_.selection();
}

void ListPanel::run_input_loop() {
    int ch;
    while ((ch = getch()) != ERR) {
        if (handle_key(ch))
            break;
    }
}

bool ListPanel::handle_key(int ch) {
    const ListState::Action act = state_.key(ch, content_rows());
    if (act == ListState::Action::Redraw)
        draw_items();
    return act == ListState::Action::Select || act == ListState::Action::Cancel;
}

void ListPanel::draw_items() {
    werase(content());
    const std::vector<std::string> f = state_.filtered();
    const int max_visible = content_rows();
    int start = 0, end = 0;
    state_.window(max_visible, start, end);

    // Scroll indicators
    if (start > 0)
        mvwaddch(content(), 0, content_cols() - 1, ACS_UARROW);
    if (end < static_cast<int>(f.size()))
        mvwaddch(content(), max_visible - 1, content_cols() - 1, ACS_DARROW);

    for (int i = start; i < end; ++i) {
        const int y = i - start;
        if (i == state_.selection()) {
            wattron(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
        } else {
            wattron(content(), COLOR_PAIR(PP_ITEM));
        }
        mvwaddnstr(content(), y, 0, f[i].c_str(), content_cols());
        if (i == state_.selection())
            wattroff(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
        else
            wattroff(content(), COLOR_PAIR(PP_ITEM));
    }

    // Filter bar (always visible at bottom of content area)
    draw_filter_bar();

    update_panels();
    doupdate();
}

void ListPanel::draw_filter_bar() {
    const int y = content_rows() - 1;
    const std::string prompt = state_.filter_mode() ? "/ " + state_.filter() : "/";
    wattron(content(), COLOR_PAIR(P_STATUS));
    mvwaddstr(content(), y, 0, std::string(content_cols(), ' ').c_str());
    mvwaddnstr(content(), y, 0, prompt.c_str(), content_cols());
    wattroff(content(), COLOR_PAIR(P_STATUS));
    if (state_.filter_mode())
        wmove(content(), y, static_cast<int>(prompt.size()));
}

} // namespace tui

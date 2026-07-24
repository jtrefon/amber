// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/list_panel.h"

namespace tui {

ListPanel::ListPanel(const std::string& title,
                     const std::vector<std::string>& items,
                     std::vector<FooterKey> footer)
    : Panel(std::min(static_cast<int>(items.size()) + 2, 20),
            std::max(static_cast<int>(title.size()) + 8, 50),
            title, footer.empty()
                ? std::vector<FooterKey>{{"Up/Down", "navigate"},
                                         {"Enter", "select"},
                                         {"Esc", "cancel"}}
                : std::move(footer)),
      items_(items) {}

int ListPanel::run() {
    draw_items();
    show();
    int ch;
    while ((ch = getch()) != ERR) {
        if (handle_key(ch)) break;
    }
    hide();
    return selection_;
}

bool ListPanel::handle_key(int ch) {
    int max_visible = content_rows();

    switch (ch) {
    case KEY_UP:
    case 'k':
        if (selection_ > 0) {
            --selection_;
            if (selection_ < scroll_offset_)
                scroll_offset_ = selection_;
            draw_items();
        }
        return false;
    case KEY_DOWN:
    case 'j':
        if (selection_ < static_cast<int>(items_.size()) - 1) {
            ++selection_;
            if (selection_ >= scroll_offset_ + max_visible)
                scroll_offset_ = selection_ - max_visible + 1;
            draw_items();
        }
        return false;
    case '\n':
    case '\r':
    case ' ':
        return true;  // select
    case 27:  // Esc
        selection_ = -1;
        return true;  // cancel
    default:
        return false;
    }
}

void ListPanel::draw_items() {
    werase(content());
    int max_visible = content_rows();
    int start = std::min(scroll_offset_, std::max(0, static_cast<int>(items_.size()) - max_visible));
    int end = std::min(start + max_visible, static_cast<int>(items_.size()));

    // Scroll indicators
    if (start > 0)
        mvwaddch(content(), 0, content_cols() - 1, ACS_UARROW);
    if (end < static_cast<int>(items_.size()))
        mvwaddch(content(), max_visible - 1, content_cols() - 1, ACS_DARROW);

    for (int i = start; i < end; ++i) {
        int y = i - start;
        if (i == selection_) {
            wattron(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
        } else {
            wattron(content(), COLOR_PAIR(PP_ITEM));
        }
        mvwaddnstr(content(), y, 0, items_[i].c_str(), content_cols());
        if (i == selection_)
            wattroff(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
        else
            wattroff(content(), COLOR_PAIR(PP_ITEM));
    }

    update_panels();
    doupdate();
}

} // namespace tui

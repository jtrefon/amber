// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/confirm_panel.h"

#include <cstring>

namespace tui {

ConfirmPanel::ConfirmPanel(const std::string& title,
                           const std::string& message)
    : Panel(7, std::max(static_cast<int>(message.size()) + 6, 40),
            title,
            {{"Tab", "switch"}, {"Enter", "confirm"}, {"Esc", "cancel"}}),
      message_(message) {}

bool ConfirmPanel::run() {
    werase(content());
    // Center the message
    int x = (content_cols() - static_cast<int>(message_.size())) / 2;
    if (x < 0) x = 0;
    mvwprintw(content(), 1, x, "%s", message_.c_str());
    draw_buttons();
    show();

    int ch;
    bool done = false;
    bool result = false;
    while (!done && (ch = getch()) != ERR) {
        switch (ch) {
        case '\t':
            yes_selected_ = !yes_selected_;
            draw_buttons();
            break;
        case KEY_LEFT:
        case KEY_RIGHT:
            yes_selected_ = !yes_selected_;
            draw_buttons();
            break;
        case '\n':
        case '\r':
            result = yes_selected_;
            done = true;
            break;
        case 27:
            done = true;
            break;
        }
    }
    hide();
    return result;
}

bool ConfirmPanel::handle_key(int ch) { (void)ch; return false; }

void ConfirmPanel::draw_buttons() {
    int total_w = 14;  // " [ Yes ]  [ No ] "
    int x = (content_cols() - total_w) / 2;
    if (x < 0) x = 0;

    // Yes button
    if (yes_selected_)
        wattron(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else
        wattron(content(), COLOR_PAIR(PP_ITEM));
    mvwprintw(content(), 3, x, " [ Yes ] ");
    if (yes_selected_)
        wattroff(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else
        wattroff(content(), COLOR_PAIR(PP_ITEM));

    // No button
    if (!yes_selected_)
        wattron(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else
        wattron(content(), COLOR_PAIR(PP_ITEM));
    mvwprintw(content(), 3, x + 8, " [ No ] ");
    if (!yes_selected_)
        wattroff(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else
        wattroff(content(), COLOR_PAIR(PP_ITEM));

    update_panels();
    doupdate();
}

} // namespace tui

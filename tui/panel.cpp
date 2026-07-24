// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/panel.h"
#include "tui/textutil.h"

#include <cstring>

namespace tui {

static int center(int parent, int child) {
    return parent > child ? (parent - child) / 2 : 0;
}

Panel::Panel(int h, int w, const std::string& title,
             std::vector<FooterKey> footer)
    : h_(h), w_(w), title_(title), footer_(std::move(footer)) {
    top_ = center(LINES, h_);
    left_ = center(COLS, w_);

    // Shadow window
    shadow_win_ = newwin(h_, w_, top_ + 1, left_ + 1);
    if (shadow_win_) {
        wbkgd(shadow_win_, COLOR_PAIR(PP_SHADOW) | ' ');
        shadow_panel_ = new_panel(shadow_win_);
    }

    // Main window
    win_ = newwin(h_, w_, top_, left_);
    wbkgd(win_, COLOR_PAIR(PP_BORDER));
    panel_ = new_panel(win_);

    // Content area (inside the border)
    content_win_ = derwin(win_, h_ - 2, w_ - 2, 1, 1);
    wbkgd(content_win_, COLOR_PAIR(PP_BORDER));

    draw_frame();
}

Panel::~Panel() {
    if (panel_) del_panel(panel_);
    if (shadow_panel_) del_panel(shadow_panel_);
    if (content_win_) delwin(content_win_);
    if (win_) delwin(win_);
    if (shadow_win_) delwin(shadow_win_);
    update_panels();
    doupdate();
}

void Panel::draw_frame() {
    if (!win_) return;

    // Clear the entire window to prevent artifacts from previous panels
    // that may have drawn on overlapping screen areas.
    werase(win_);

    // Border with ACS lines
    wborder(win_, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE,
            ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);

    // Title in top border
    if (!title_.empty()) {
        int t = (w_ - static_cast<int>(title_.size())) / 2;
        if (t < 2) t = 2;
        mvwaddch(win_, 0, t - 1, ACS_VLINE);
        // Erase the title area (overwrite the HLINEs)
        for (size_t i = 0; i < title_.size(); ++i)
            mvwaddch(win_, 0, t + static_cast<int>(i), ' ');
        mvwprintw(win_, 0, t, "%s", title_.c_str());
        t += static_cast<int>(title_.size());
        mvwaddch(win_, 0, t, ACS_VLINE);
    }

    // Footer with keyboard shortcuts — clear the entire bottom line first
    // to avoid rendering artifacts from previous panels with longer footers.
    mvwhline(win_, h_ - 1, 1, ' ', w_ - 2);
    if (!footer_.empty()) {
        int pos = 2;
        for (const auto& f : footer_) {
            // Key in bold/attribute
            wattron(win_, A_BOLD | COLOR_PAIR(PP_FOOTER));
            pos += mvwprintw(win_, h_ - 1, pos, " [%s]", f.key.c_str());
            wattroff(win_, A_BOLD | COLOR_PAIR(PP_FOOTER));
            // Action label
            wattron(win_, COLOR_PAIR(PP_FOOTER));
            pos += printw(" %s ", f.action.c_str());
            wattroff(win_, COLOR_PAIR(PP_FOOTER));
        }
    }

    // Redraw the corners (wborder draws them but we overwrote the top ones)
    mvwaddch(win_, 0, 0, ACS_ULCORNER);
    mvwaddch(win_, 0, w_ - 1, ACS_URCORNER);
    mvwaddch(win_, h_ - 1, 0, ACS_LLCORNER);
    mvwaddch(win_, h_ - 1, w_ - 1, ACS_LRCORNER);
}

void Panel::show() {
    if (panel_) show_panel(panel_);
    if (shadow_panel_) show_panel(shadow_panel_);
    draw_frame();
    update_panels();
    doupdate();
}

void Panel::hide() {
    if (panel_) hide_panel(panel_);
    if (shadow_panel_) hide_panel(shadow_panel_);
    update_panels();
    doupdate();
}

bool Panel::handle_key(int ch) {
    if (ch == KEY_F(1)) {
        show_help();
        draw_frame();
        return true;
    }
    return false;
}

void Panel::show_help() {
    // Build help text from footer keys
    std::vector<std::string> help_lines;
    help_lines.push_back(" Available keys:");
    help_lines.push_back("");
    for (const auto& f : footer_) {
        help_lines.push_back("  [" + f.key + "] " + f.action);
    }
    if (footer_.empty()) {
        help_lines.push_back("  [Esc] Close");
    }
    help_lines.push_back("");
    help_lines.push_back("  [F1] This help");
    help_lines.push_back("  [Esc] Close");

    int h = static_cast<int>(help_lines.size()) + 2;
    int w = 40;
    int sh, sw;
    getmaxyx(stdscr, sh, sw);
    int y = (sh - h) / 2;
    int x = (sw - w) / 2;

    WINDOW* help_win = newwin(h, w, y, x);
    wbkgd(help_win, COLOR_PAIR(P_DIALOG));
    box(help_win, 0, 0);
    mvwaddstr(help_win, 0, 2, " Help ");
    for (int i = 0; i < static_cast<int>(help_lines.size()); ++i)
        mvwaddnstr(help_win, 1 + i, 1, help_lines[i].c_str(), w - 2);
    wrefresh(help_win);
    // Wait for a keypress then dismiss
    wgetch(help_win);
    delwin(help_win);
    touchwin(win_);
}

} // namespace tui

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
                                         {"/", "filter"},
                                         {"Esc", "cancel"}}
                : std::move(footer)),
      items_(items) {}

std::vector<std::string> ListPanel::filtered() const {
    if (filter_.empty()) return items_;
    std::vector<std::string> out;
    for (const auto& item : items_)
        if (item.find(filter_) != std::string::npos)
            out.push_back(item);
    return out;
}

int ListPanel::run() {
    draw_items();
    show();
    int ch;
    while ((ch = getch()) != ERR) {
        if (handle_key(ch)) break;
    }
    hide();
    if (!filter_.empty()) {
        // If filtering, map selected index to original items_
        auto f = filtered();
        if (selection_ >= 0 && selection_ < static_cast<int>(f.size())) {
            for (int i = 0; i < static_cast<int>(items_.size()); ++i)
                if (items_[i] == f[selection_])
                    { selection_ = i; break; }
        }
    }
    return selection_;
}

bool ListPanel::handle_key(int ch) {
    if (ch == '/') {
        filter_mode_ = true;
        filter_.clear();
        selection_ = 0;
        scroll_offset_ = 0;
        draw_items();
        return false;
    }

    if (filter_mode_) {
        if (ch == 27) {  // Esc cancels filter
            filter_mode_ = false;
            filter_.clear();
            selection_ = 0;
            scroll_offset_ = 0;
            draw_items();
            return false;
        }
        if ((ch == KEY_BACKSPACE || ch == 127) && !filter_.empty()) {
            filter_.pop_back();
            selection_ = 0;
            scroll_offset_ = 0;
            draw_items();
            return false;
        }
        if (ch >= 32 && ch < 127) {
            filter_ += static_cast<char>(ch);
            selection_ = 0;
            scroll_offset_ = 0;
            draw_items();
            return false;
        }
        // During filter mode, Enter/Esc still work for selection/cancel
        if (ch == '\n' || ch == '\r') { filter_mode_ = false; return true; }
        if (ch == 27) { filter_mode_ = false; return true; }
        return false;
    }

    int max_visible = content_rows();
    auto f = filtered();

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
        if (selection_ < static_cast<int>(f.size()) - 1) {
            ++selection_;
            if (selection_ >= scroll_offset_ + max_visible)
                scroll_offset_ = selection_ - max_visible + 1;
            draw_items();
        }
        return false;
    case '\n':
    case '\r':
    case ' ':
        filter_mode_ = false;
        return true;  // select
    case 27:  // Esc
        filter_mode_ = false;
        selection_ = -1;
        return true;  // cancel
    default:
        return false;
    }
}

void ListPanel::draw_items() {
    werase(content());
    auto f = filtered();
    int max_visible = content_rows();
    int start = std::min(scroll_offset_, std::max(0, static_cast<int>(f.size()) - max_visible));
    int end = std::min(start + max_visible, static_cast<int>(f.size()));

    // Scroll indicators
    if (start > 0)
        mvwaddch(content(), 0, content_cols() - 1, ACS_UARROW);
    if (end < static_cast<int>(f.size()))
        mvwaddch(content(), max_visible - 1, content_cols() - 1, ACS_DARROW);

    for (int i = start; i < end; ++i) {
        int y = i - start;
        if (i == selection_) {
            wattron(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
        } else {
            wattron(content(), COLOR_PAIR(PP_ITEM));
        }
        mvwaddnstr(content(), y, 0, f[i].c_str(), content_cols());
        if (i == selection_)
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
    int y = content_rows() - 1;
    std::string prompt = filter_mode_ ? "/ " + filter_ : "/";
    wattron(content(), COLOR_PAIR(P_STATUS));
    mvwaddstr(content(), y, 0, std::string(content_cols(), ' ').c_str());
    mvwaddnstr(content(), y, 0, prompt.c_str(), content_cols());
    wattroff(content(), COLOR_PAIR(P_STATUS));
    if (filter_mode_)
        wmove(content(), y, static_cast<int>(prompt.size()));
}

} // namespace tui

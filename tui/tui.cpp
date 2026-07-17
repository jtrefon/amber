// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"

#include <agent.h>

#include "widgets.h"
#include "textutil.h"
#include "welcome.h"

#include <clocale>
#include <ctime>
#include <functional>

namespace tui {

Tui::Tui(agent::Config cfg, agent::ToolRegistry& reg)
    : cfg_(std::move(cfg)), reg_(reg), store_() {
    std::setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(1);
    start_color();
    use_default_colors();
    use_legacy_coding(1);   // Unicode line drawing on macOS Terminal
    init_pairs();
    open_welcome_window();
}

Tui::~Tui() {
    for (size_t i = 0; i < windows_.size(); ++i) {
        Window& w = *windows_[i];
        if (!w.dirty || !w.agent || w.agent->history().empty()) continue;
        agent::Session s = snapshot(w);
        if (store_.save(s)) w.session_id = s.id;
    }
    endwin();
}

Window& Tui::new_window(const std::string& title) {
    auto w = std::make_unique<Window>();
    w->title = title;
    w->agent = std::make_unique<agent::Agent>(cfg_, reg_);
    windows_.push_back(std::move(w));
    active_ = windows_.size() - 1;
    return *windows_.back();
}

Window& Tui::open_welcome_window() {
    auto w = std::make_unique<Window>();
    w->title = "amber";
    w->read_only = true;
    w->welcome_art = true;
    windows_.push_back(std::move(w));
    active_ = windows_.size() - 1;
    return *windows_.back();
}

Window& Tui::ensure_chat_window() {
    if (!win().read_only) return win();
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (!windows_[i]->read_only) { switch_to(i); return win(); }
    }
    return new_window("chat");
}

Window& Tui::win() { return *windows_[active_]; }
const Window& Tui::win() const { return *windows_[active_]; }

void Tui::run() {
    draw();
    draw_input("");
    detect_server(false);

    timeout(1000);

    std::string input;
    while (!quit_) {
        int ch = getch();
        if (ch == ERR) { tick_clock(); draw_input(input); continue; }
        if (ch == 14) {
            new_window("chat");
            draw(); draw_input(input); continue;
        }
        if (ch == 23) {
            close_window();
            draw(); draw_input(input); continue;
        }
        if (ch == 27) {
            if (drawer_open_) {
                drawer_open_ = false;
                draw(); draw_input(input);
                continue;
            }
            int n = getch();
            if (n >= '1' && n <= '9') {
                switch_to(static_cast<size_t>(n - '1'));
                draw_input(input);
            }
            continue;
        }
        if (ch == '\t' && drawer_open_) {
            input = drawer_complete(input);
            drawer_sel_ = 0;
            draw(); draw_input(input);
            continue;
        }
        if (drawer_open_ && (ch == KEY_UP || ch == KEY_DOWN)) {
            int n = static_cast<int>(filter_commands(drawer_token(input)).size());
            if (n > 0) {
                if (ch == KEY_DOWN) drawer_sel_ = (drawer_sel_ + 1) % n;
                else drawer_sel_ = (drawer_sel_ + n - 1) % n;
            }
            draw_input(input);
            continue;
        }
        if (ch == KEY_NPAGE) {
            win().scroll_top = std::min(max_scroll(),
                                   win().scroll_top + lines_per_page());
            draw(); draw_input(input); continue;
        }
        if (ch == KEY_PPAGE) {
            win().scroll_top = std::max(0, win().scroll_top - lines_per_page());
            draw(); draw_input(input); continue;
        }
        if (ch == 7 || ch == 10 || ch == 13 || ch == KEY_ENTER) {
            if (drawer_open_ && !drawer_has_arg(input)) {
                auto matches = filter_commands(drawer_token(input));
                if (!matches.empty() &&
                    drawer_sel_ < static_cast<int>(matches.size())) {
                    const Command* c = matches[drawer_sel_];
                    drawer_open_ = false;
                    input.clear();
                    handle_slash("/" + c->name);
                    draw(); draw_input("");
                    continue;
                }
            }
            if (input.empty()) continue;
            std::string prompt = input;
            drawer_open_ = false;
            if (handle_slash(prompt)) {
                input.clear();
                draw(); draw_input("");
                continue;
            }
            ensure_chat_window();
            append_line(P_USER, "> " + input);
            input.clear();
            draw(); draw_input("");
            send(prompt);
            draw_input("");
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (!input.empty()) input.pop_back();
            update_drawer(input);
            draw(); draw_input(input);
        } else if (ch >= 32 && ch <= 126) {
            input += static_cast<char>(ch);
            update_drawer(input);
            draw(); draw_input(input);
        }
    }
}

} // namespace tui

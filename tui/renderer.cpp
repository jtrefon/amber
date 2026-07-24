// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/renderer.h"
#include "tui/chat_view.h"
#include "tui/status_bar.h"

#include <ncurses.h>

namespace tui {

Renderer::Renderer(std::shared_ptr<EventBus> bus) : bus_(std::move(bus)) {
    init_ncurses();
}

Renderer::~Renderer() {
    if (ncurses_initialized_) endwin();
}

void Renderer::init_ncurses() {
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(0);
    ncurses_initialized_ = true;
}

void Renderer::draw(const Layout& layout, const std::string& input,
                     bool drawer_open) {
    if (!ncurses_initialized_) return;
    erase();

    // Draw chat area
    if (chat_) {
        // ChatView paints into the chat region
    }

    // Draw input line
    mvaddstr(layout.input_top, 0, input.c_str());
    clrtoeol();

    // Draw status bar
    if (status_) {
        mvaddstr(layout.status_top, 0, status_->render().c_str());
        clrtoeol();
    }

    refresh();
}

} // namespace tui

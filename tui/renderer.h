// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_RENDERER_H
#define AMBER_TUI_RENDERER_H

#include "tui/event_bus.h"
#include "tui/layout.h"

#include <memory>
#include <string>
#include <vector>

namespace tui {

class ChatView;
class StatusBar;

// ncurses renderer. Draws all components in order, then flushes once.
// All ncurses calls happen here — no other component touches curses.
class Renderer {
public:
    explicit Renderer(std::shared_ptr<EventBus> bus);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    struct Seg {
        std::string text;
        int pair = 0;
        int drop = 0;
    };

    void set_chat_view(ChatView* v) { chat_ = v; }
    void set_status_bar(StatusBar* s) { status_ = s; }

    // Called once per frame by App.
    void draw(const Layout& layout, const std::string& input, bool drawer_open);

private:
    std::shared_ptr<EventBus> bus_;
    ChatView* chat_ = nullptr;
    StatusBar* status_ = nullptr;
    bool ncurses_initialized_ = false;

    void init_ncurses();
};

} // namespace tui

#endif // AMBER_TUI_RENDERER_H

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/input_handler.h"

#include <ncurses.h>

namespace tui {

InputHandler::InputHandler(std::shared_ptr<EventBus> bus) : bus_(std::move(bus)) {
    bus_->on<PromptSubmitted>([this](const PromptSubmitted& e) {
        if (e.text.empty()) return;
        // The prompt will be picked up by AgentService
    });
}

void InputHandler::poll() {
    int ch = getch();
    if (ch == ERR) return;
    handle_char(ch);
}

void InputHandler::handle_char(int ch) {
    if (ch == '\n' || ch == '\r') {
        handle_enter();
        return;
    }
    if (ch == 27) {  // Escape
        bus_->emit(CancelRequested{});
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127) {
        if (!input_.empty()) input_.pop_back();
        return;
    }
    if (ch >= 32 && ch < 127) {
        input_ += static_cast<char>(ch);
    }
}

void InputHandler::handle_enter() {
    if (input_.empty()) return;
    std::string text = input_;
    input_.clear();
    bus_->emit(PromptSubmitted{text});
}

} // namespace tui

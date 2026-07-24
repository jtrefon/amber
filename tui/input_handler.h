// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_INPUT_HANDLER_H
#define AMBER_TUI_INPUT_HANDLER_H

#include "tui/event_bus.h"

#include <memory>
#include <string>

namespace tui {

// Reads keyboard input, builds the input buffer, emits UserInput on Enter.
class InputHandler {
public:
    explicit InputHandler(std::shared_ptr<EventBus> bus);

    const std::string& input() const { return input_; }
    bool drawer_open() const { return drawer_open_; }

    // Poll keyboard (non-blocking, ~50ms timeout). Called once per frame.
    void poll();

private:
    std::shared_ptr<EventBus> bus_;
    std::string input_;
    bool drawer_open_ = false;
    int drawer_sel_ = 0;

    void handle_char(int ch);
    void handle_enter();
};

} // namespace tui

#endif // AMBER_TUI_INPUT_HANDLER_H

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_CHAT_VIEW_H
#define AMBER_TUI_CHAT_VIEW_H

#include "tui/event_bus.h"
#include "tui/rich.h"
#include "tui/markdown.h"

#include <memory>
#include <string>
#include <vector>

namespace tui {

// Owns the conversation scrollback and streaming state.
class ChatView {
public:
    ChatView(std::shared_ptr<EventBus> bus, md::Style style);

    const std::vector<rich::Line>& lines() const { return lines_; }
    int scroll_pos() const { return scroll_pos_; }
    void set_scroll_pos(int s) { scroll_pos_ = s; }

private:
    std::vector<rich::Line> lines_;
    int scroll_pos_ = 0;
    std::string stream_buf_;
    std::string reason_buf_;

    void on_token(const TokenReceived& e);
    void on_reasoning(const ReasoningReceived& e);
    void on_assistant(const AssistantDone& e);
    void on_status(const StatusMessage& e);
};

} // namespace tui

#endif // AMBER_TUI_CHAT_VIEW_H

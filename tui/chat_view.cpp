// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/chat_view.h"

namespace tui {

ChatView::ChatView(std::shared_ptr<EventBus> bus, md::Style style) {
    bus->on<TokenReceived>([this](const TokenReceived& e) { on_token(e); });
    bus->on<ReasoningReceived>([this](const ReasoningReceived& e) { on_reasoning(e); });
    bus->on<AssistantDone>([this](const AssistantDone& e) { on_assistant(e); });
    bus->on<StatusMessage>([this](const StatusMessage& e) { on_status(e); });
    (void)style;
}

void ChatView::on_token(const TokenReceived& e) {
    stream_buf_ += e.text;
}

void ChatView::on_reasoning(const ReasoningReceived& e) {
    reason_buf_ += e.text;
}

void ChatView::on_assistant(const AssistantDone& e) {
    // Flush stream buffer as a rich line
    if (!stream_buf_.empty()) {
        rich::Line l;
        rich::Run r;
        r.text = stream_buf_;
        l.runs.push_back(r);
        lines_.push_back(l);
        stream_buf_.clear();
    }
    reason_buf_.clear();
}

void ChatView::on_status(const StatusMessage& e) {
    (void)e;
}

} // namespace tui

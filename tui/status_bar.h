// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_STATUS_BAR_H
#define AMBER_TUI_STATUS_BAR_H

#include "tui/event_bus.h"

#include <memory>
#include <string>

namespace tui {

// Bottom status line: state, stats, clock, running tool.
class StatusBar {
public:
    explicit StatusBar(std::shared_ptr<EventBus> bus);

    std::string render() const;

private:
    agent::RunState state_ = agent::RunState::Idle;
    agent::Stats stats_;
    std::string status_text_;
    std::string running_tool_;

    void on_state(const StateChanged& e);
    void on_stats(const StatsUpdated& e);
    void on_status(const StatusMessage& e);
};

} // namespace tui

#endif // AMBER_TUI_STATUS_BAR_H

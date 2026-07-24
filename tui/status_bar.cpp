// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/status_bar.h"
#include "tui/glyphs.h"
#include "agent/statusbar.h"

#include <sstream>

namespace tui {

StatusBar::StatusBar(std::shared_ptr<EventBus> bus) {
    bus->on<StateChanged>([this](const StateChanged& e) { on_state(e); });
    bus->on<StatsUpdated>([this](const StatsUpdated& e) { on_stats(e); });
    bus->on<StatusMessage>([this](const StatusMessage& e) { on_status(e); });
}

std::string StatusBar::render() const {
    std::ostringstream out;
    // Format: [model] mode [tool] lag Ns  t/s  ctx gauge [clock]
    auto g = glyph::get();
    out << "[" << model_ << "] " << status_text_;
    return out.str();
}

void StatusBar::on_state(const StateChanged& e) { state_ = e.state; }
void StatusBar::on_stats(const StatsUpdated& e) { stats_ = e.stats; }
void StatusBar::on_status(const StatusMessage& e) { status_text_ = e.text; }

} // namespace tui

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#pragma once

#include <agent.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "widgets.h"

namespace tui {

// One chat window: an independent conversation with its own scrollback, live
// streaming state, persistent (stateful) Agent, and session identity. Windows
// are switchable IRC-style; the active one is drawn.
struct Window {
    std::string title = "chat";
    std::string session_id;          // set once persisted / loaded
    bool dirty = false;              // has unsaved changes since last save

    std::unique_ptr<agent::Agent> agent;  // retains conversation across turns

    std::vector<std::pair<int, std::string>> lines;
    int scroll_top = 0;

    std::string stream_buf;
    int stream_color = P_ASSISTANT;
    std::string stream_ts;
    std::string reason_buf;
    bool reason_folded = false;
};

}  // namespace tui

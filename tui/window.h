// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#pragma once

#include <agent.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rich.h"
#include "widgets.h"

namespace tui {

// How tool calls and results are displayed in the scrollback.
enum class ToolFold { Always, Auto, Never };

// One chat window: an independent conversation with its own scrollback, live
// streaming state, persistent (stateful) Agent, and session identity. Windows
// are switchable IRC-style; the active one is drawn.
struct Window {
    std::string title = "chat";
    std::string session_id;          // set once persisted / loaded
    bool dirty = false;              // has unsaved changes since last save
    bool read_only = false;          // welcome / log window: typing spawns chat
    bool welcome_art = false;        // renders via welcome::render() instead of lines

    std::unique_ptr<agent::Agent> agent;  // retains conversation across turns

    // Scrollback as rich (multi-run) lines. Markdown assistant messages and
    // styled tool/status lines both live here; the canvas wraps and renders
    // them. Replaces the old vector<pair<int,string>> of plain lines.
    std::vector<rich::Line> lines;
    int scroll_top = 0;

    bool markdown_on = true;          // render assistant text as Markdown
    std::string stream_buf;           // raw streamed assistant markdown (live)
    int stream_color = P_ASSISTANT;
    std::string stream_ts;
    std::string reason_buf;
    bool reason_folded = false;
};

}  // namespace tui

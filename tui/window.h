
#pragma once

#include <agent.h>

#include "agent/extensions.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "reasoning_block.h"
#include "usage_ledger.h"
#include "rich.h"
#include "widgets.h"

namespace tui {

// One chat window: an independent conversation with its own scrollback, live
// streaming state, persistent (stateful) Agent, and session identity. Windows
// are switchable IRC-style; the active one is drawn.
struct Window {
    enum class Kind { Chat, Resource };
    Kind kind = Kind::Chat;
    std::string resource_kind;
    std::string resource_id;
    // Stable identity: assigned monotonically by the Tui at creation and
    // never reused. AgentEvent::window_id and PendingToolLine::window_id
    // carry this value so events follow their window across windows_ erasures
    // (a vector index would shift when another window is closed).
    size_t id = std::string::npos; // unassigned until the Tui sets it
    // Stable identity for background report routing. Unlike id, this is
    // persisted with the workspace and survives a restart; unlike session_id,
    // it exists before the first save.
    std::string owner_id;
    // The project this window belongs to. Empty means the process default.
    std::string project_root;
    std::string title = "chat";
    std::string session_id;         // set once persisted / loaded
    std::atomic<bool> dirty{false}; // has unsaved changes since last save
    bool read_only = false;         // welcome / log window: typing spawns chat
    // Resource viewers only: set once the user has actually looked at the
    // window, so a terminal state keeps the final output instead of closing
    // under them.
    bool ever_focused = false;
    bool welcome_art = false; // renders via welcome::render() instead of lines

    // This window's workspace: the worktree its tools operate in, empty when it
    // works in the project root. base_oid is the commit the worktree was cut
    // from — that is what rebuilds it exactly after /tmp is wiped, because the
    // commits live in the repository, not in the worktree.
    std::string root;
    std::string worktree;
    std::string base_oid;

    std::unique_ptr<agent::Agent> agent; // retains conversation across turns

    // Scrollback as rich (multi-run) lines. Markdown assistant messages and
    // styled tool/status lines both live here; the canvas wraps and renders
    // them. Replaces the old vector<pair<int,string>> of plain lines.
    std::vector<rich::Line> lines;
    int scroll_top = 0;
    std::vector<std::string> prompt_history;
    size_t history_pos = 0; // 0 = end (no recall), > 0 = recalling older entries

    bool markdown_on = true; // render assistant text as Markdown
    std::string stream_buf;  // raw streamed assistant markdown (live)
    int stream_color = P_ASSISTANT;
    std::string stream_ts;
    ReasoningBlock reason; // live thinking block (folds when done)

    // ---- per-window run state -------------------------------------------
    // "Agent is running" is a per-window fact: each window's agent has its
    // own worker (RunRegistry, keyed by id). Everything a worker reports
    // lives here so two concurrent runs never corrupt each other's status.
    agent::RunState state = agent::RunState::Idle;
    agent::Stats stats;
    // Session spend, accumulated from every completed exchange (see
    // UsageTotals: it spans compactions and is what the token segment shows;
    // ctx_used below is what the NEXT request sends).
    // Source-split spend; `usage` stays the tree total for the status bar.
    UsageLedger ledger;
    agent::UsageTotals usage;
    // Context token gauge. Written by this window's worker via context
    // events / stats and by the UI thread on session load; read by the UI
    // thread for the status bar. ctx_used is the server-reported
    // prompt_tokens (-1 until known); ctx_estimate is the live chars/4
    // estimate; gauge_tokens() picks the single source.
    std::atomic<long> ctx_used{-1};
    std::atomic<long> ctx_estimate{0};
    long live_ctx_offset = 0; // running token count during streaming
    std::string running_tool; // in-flight tool name (working verb)
    std::string running_tool_desc;
    bool compressing = false; // context compression in flight
};

} // namespace tui

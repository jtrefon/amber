// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_TUI_H
#define AMBER_TUI_TUI_H

#include <agent.h>

#include "widgets.h"
#include "textutil.h"
#include "window.h"
#include "palette.h"
#include "rich.h"
#include "canvas.h"
#include "markdown.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace agent {
class ToolRegistry;
} // namespace agent

namespace tui {
using palette::Command;

// Inter-thread event emitted by the agent worker and consumed on the UI
// thread during the main event loop.
struct AgentEvent {
    enum Type {
        Token,
        Reasoning,
        StateChange,
        ToolCall,
        ToolResult,
        Status,
        Stats,
        Assistant,
        Approval,
        Error,
        Done,
    };
    Type type;
    std::string text;
    agent::RunState state = agent::RunState::Idle;
    agent::Stats stats{};
    std::string tool_name;
    agent::ToolResult tool_result{};
    agent::json tool_args;
    std::string error_msg;

    // Worker thread blocks on this promise until the UI thread
    // shows the approval dialog and resolves it.
    std::shared_ptr<std::promise<agent::Approval>> approval_promise;
};

// ncurses-based interactive TUI. Operates an IRC-style multi-window chat
// interface on top of the agent core. One instance per process; the main
// function creates it and calls run().
class Tui {
public:
    Tui(agent::Config cfg, agent::ToolRegistry& reg, agent::JobService& jobs);
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    Window& new_window(const std::string& title);
    Window& open_welcome_window();
    Window& ensure_chat_window();
    void run();

private:
    // ---- thread / event machinery ---------------------------------------
    bool drain_events();       // pop and process all pending events
    void pump_events();        // drain + redraw; safe to call from a modal loop
    void resolve_approval(const AgentEvent& ev);
    void send_async(const std::string& prompt);
    void agent_worker(const std::string& prompt);

    std::queue<AgentEvent> event_queue_;
    std::mutex event_mtx_;
    std::thread agent_thread_;
    std::atomic<bool> agent_busy_{false};
    std::atomic<bool> agent_cancel_{false};

    // Name of the tool currently executing on the agent worker (foreground,
    // e.g. bash), surfaced on the status bar so a synchronous command that is
    // not a JobService background job is still visible while it runs.
    std::string running_tool_;
    // A prompt the user typed (and submitted with Enter) while the agent was
    // busy; auto-sent once the agent returns to idle so typing never feels
    // blocked.
    std::string pending_prompt_;

    // A modal dialog (info_dialog / menu_select / config / session browser)
    // blocks the main thread in wgetch, so drain_events() cannot run. If the
    // agent needs an approval while a modal is open we queue it and resolve it
    // once the modal closes, instead of nesting ncurses dialogs or deadlocking
    // the worker on its promise.
    bool modal_open_ = false;
    std::queue<AgentEvent> pending_approvals_;
    palette::Completer completer_;  // Tab-press state machine

    // ---- geometry / layout ----------------------------------------------
    int height() const;
    int width() const;
    int chat_top() const;
    int chat_height() const;
    int lines_per_page() const;
    int stream_lines() const;
    int max_scroll() const;

    // ---- low-level helpers ----------------------------------------------
    void redraw(const std::string& input);
    static size_t utf8_len(const std::string& s, size_t i);
    static std::vector<std::string> wrap_text(const std::string& text, int w);
    static std::string timestamp();
    void append_line(int color, const std::string& text);
    void append_line_ts(int color, const std::string& text,
                        const std::string& ts);
    void append_rich(const rich::Line& l);
    void append_markdown(const std::string& md);
    // Append a plain color run as a wrapped RichLine into an existing view
    // vector (used for the live stream preview inside draw()).
    static void append_rich_to(std::vector<rich::Line>& view,
                               const std::string& text, int color, int w);
    void banner(const std::string& text);
    void trim_lines();

    // ---- rendering ------------------------------------------------------
    // Stage-only redraw helpers write to stdscr without flushing; flush()
    // performs the single physical update per tick (ncurses best practice:
    // wnoutrefresh + one doupdate, instead of per-call refresh() that forces a
    // full-screen repaint on every change and causes flicker).
    void flush() { doupdate(); }

    struct Seg {
        std::string text;
        int pair = tui::P_BANNER;
        int drop = 0;
    };
    static int display_cols(const std::string& s);
    static std::wstring to_wide(const std::string& s);
    static std::string kfmt(long n);
    static int gauge_pair(double f);
    std::vector<Seg> bar_segments() const;
    void draw();
    void draw_status_bar(const std::string& tail);
    void tick_clock();
    void draw_input(const std::string& s);
    void draw_drawer(const std::string& input);
    int drawer_menu(const std::string& title,
                    const std::vector<std::string>& items);

    // ---- command drawer -------------------------------------------------
    void update_drawer(const std::string& input);
    static std::string drawer_token(const std::string& input);
    static bool drawer_has_arg(const std::string& input);
    std::vector<const tui::Command*> filter_commands(const std::string& token);
    bool drawer_wants_open(const std::string& input) const;

    // ---- streaming helpers ----------------------------------------------
    void fold_reasoning();
    void flush_stream();

    // ---- session persistence --------------------------------------------
    agent::Session snapshot(Window& w) const;
    void autosave();
    void save_session();
    void load_session(const std::string& id);
    void session_browser();
    void pick_session();

    // ---- window management ----------------------------------------------
    void switch_to(size_t idx);
    void lazy_load_active();
    void close_window();
    Window& win();
    const Window& win() const;

    // ---- slash command framework ----------------------------------------
    const std::vector<tui::Command>& commands();
    void build_commands();
    ToolFold tool_fold_ = ToolFold::Auto;  // global tool-call display mode
    const tui::Command* find_command(const std::string& name);
    bool handle_slash(const std::string& line);
    std::string usage(const tui::Command& c) const;
    void show_command_frame(const tui::Command& c);
    void cmd_help(const std::string& arg);
    void cmd_window(const std::string& arg);
    void cmd_job(const std::string& arg);
    void cmd_compress(const std::string& arg);
    void cmd_set(const std::string& arg);
    void job_ls();
    void job_kill(const std::string& id);
    void job_read(const std::string& id);
    void job_start(const std::string& cmd);
    void request_quit();
public:
    void save_workspace_now();
    void redraw_after_modal();
    void toggle_thinking();
    void cmd_policy(const std::string& arg);
    void cmd_toolfold(const std::string& arg);
    void cmd_display(const std::string& arg);
    void config_screen() const;
    void detect_server(bool force);
    bool test_connection(bool announce);
    void settings_screen();
    void save_settings();
    void send(const std::string& prompt);

    // ---- member variables -----------------------------------------------
    agent::Config cfg_;
    agent::ToolRegistry& reg_;
    agent::JobService& jobs_;       // host-owned; shared with process_* tools
    agent::SessionStore store_;
    std::string settings_path_;

    std::vector<std::unique_ptr<Window>> windows_;
    size_t active_ = 0;

    Canvas chat_canvas_;                 // dedicated chat scrollback window
    md::Style md_style_;                 // markdown color mapping

    std::vector<tui::Command> commands_;
    bool quit_ = false;

    bool drawer_open_ = false;
    int drawer_sel_ = 0;
    bool show_reasoning_ = true;

    agent::RunState state_ = agent::RunState::Idle;
    agent::Stats stats_;
    long ctx_used_ = -1;
    long live_ctx_offset_ = 0;   // running token count during streaming
    agent::ServerInfo last_detected_;
    int anim_phase_ = 0;
    bool dirty_ = true;          // coalesce redraws into one flush per tick
    std::string last_input_;     // for change detection on idle ticks
    // Wall-clock timestamp of the last status-bar repaint, so the clock and
    // progress wave keep ticking while the agent is blocked on a call that
    // emits no streaming tokens.
    std::chrono::steady_clock::time_point last_status_tick_{};
};

} // namespace tui

#endif

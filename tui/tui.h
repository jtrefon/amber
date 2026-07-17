// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_TUI_H
#define AMBER_TUI_TUI_H

#include <agent.h>

#include "widgets.h"
#include "textutil.h"
#include "window.h"
#include "palette.h"

#include <atomic>
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
    Tui(agent::Config cfg, agent::ToolRegistry& reg);
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
    void send_async(const std::string& prompt);
    void agent_worker(const std::string& prompt);

    std::queue<AgentEvent> event_queue_;
    std::mutex event_mtx_;
    std::thread agent_thread_;
    std::atomic<bool> agent_busy_{false};
    std::atomic<bool> agent_cancel_{false};

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
    void banner(const std::string& text);

    // ---- rendering ------------------------------------------------------
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
    std::string drawer_complete(const std::string& input);

    // ---- streaming helpers ----------------------------------------------
    void fold_reasoning();
    void flush_stream();

    // ---- session persistence --------------------------------------------
    agent::Session snapshot(Window& w);
    void autosave();
    void save_session();
    void load_session(const std::string& id);
    void session_browser();
    void pick_session();

    // ---- window management ----------------------------------------------
    void switch_to(size_t idx);
    void close_window();
    Window& win();
    const Window& win() const;

    // ---- slash command framework ----------------------------------------
    const std::vector<tui::Command>& commands();
    void build_commands();
    const tui::Command* find_command(const std::string& name);
    bool handle_slash(const std::string& line);
    std::string usage(const tui::Command& c) const;
    void cmd_help(const std::string& arg);
    void cmd_window(const std::string& arg);
    void request_quit();
    void redraw_after_modal();
    void toggle_thinking();
    void cmd_policy(const std::string& arg);
    void config_screen();
    void detect_server(bool force);
    bool test_connection(bool announce);
    void settings_screen();
    void save_settings();
    void send(const std::string& prompt);

    // ---- member variables -----------------------------------------------
    agent::Config cfg_;
    agent::ToolRegistry& reg_;
    agent::SessionStore store_;
    std::string settings_path_ = "amber.conf";

    std::vector<std::unique_ptr<Window>> windows_;
    size_t active_ = 0;

    std::vector<tui::Command> commands_;
    bool quit_ = false;

    bool drawer_open_ = false;
    int drawer_sel_ = 0;
    bool show_reasoning_ = true;

    agent::RunState state_ = agent::RunState::Idle;
    agent::Stats stats_;
    long ctx_used_ = -1;
    agent::ServerInfo last_detected_;
    int anim_phase_ = 0;
};

} // namespace tui

#endif

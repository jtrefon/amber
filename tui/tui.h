
#ifndef AMBER_TUI_TUI_H
#define AMBER_TUI_TUI_H

#include <agent.h>
#include <agent/learn_commands.h>
#include <agent/mcp_config.h>
#include <agent/model_probe.h>
#include <agent/plugin.h>

#include "widgets.h"
#include "textutil.h"
#include "window.h"
#include "palette.h"
#include "rich.h"
#include "canvas.h"
#include "markdown.h"
#include "action_registry.h"
#include "setting_registry.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <set>
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
        CompressResult,
    };
    Type type;
    std::string text;
    agent::RunState state = agent::RunState::Idle;
    agent::Stats stats{};
    std::string tool_name;
    agent::ToolResult tool_result{};
    agent::json tool_args;
    std::string error_msg;
    agent::CompressionResult compress_result{};

    // Worker thread blocks on this promise until the UI thread
    // shows the approval dialog and resolves it.
    std::shared_ptr<std::promise<agent::Approval>> approval_promise;
};

// ncurses-based interactive TUI. Operates an IRC-style multi-window chat
// interface on top of the agent core. One instance per process; the main
// function creates it and calls run().
class Tui {
public:
    Tui(agent::Config cfg, agent::ToolRegistry& reg, agent::JobService& jobs,
        agent::SubAgentExecutor& subagents, agent::PluginManager& plugins);
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

    void resolve_approval(const AgentEvent& ev);
    void send_async(const std::string& raw_prompt);
    std::string expand_at_references(const std::string& raw) const;
    void agent_worker(const std::string& prompt);
    void compress_worker();

    std::queue<AgentEvent> event_queue_;
    std::mutex event_mtx_;
    std::thread agent_thread_;
    std::atomic<bool> agent_busy_{false};
    std::atomic<bool> agent_cancel_{false};

    // Name of the tool currently executing on the agent worker (foreground,
    // e.g. bash), surfaced on the status bar so a synchronous command that is
    // not a JobService background job is still visible while it runs.
    std::string running_tool_;
    // Human-readable description of the running tool call (describe_tool_call
    // output), shown in the sticky working row while the tool executes.
    std::string running_tool_desc_;

    // One advertised tool call = one pending scrollback line. All advertised
    // calls animate together (the round spinner glyph is swapped in-place
    // each 150ms tick); each line's icon flips to success/failure when its
    // result lands. Scrollback-only — the agent's immutable context stays
    // sealed.
    struct PendingToolLine {
        size_t index = std::string::npos;  // index into win().lines
        std::string name;
        std::string fingerprint;  // args dump — identity for result matching
        std::string tail;         // " <description>" after the spinner glyph
        int frame = 0;
    };
    std::vector<PendingToolLine> pending_tools_;
    void advance_tool_spinners();
    // A prompt the user typed (and submitted with Enter) while the agent was
    // busy; auto-sent once the agent returns to idle so typing never feels
    // blocked.
    std::string pending_prompt_;

    // ---- git state (decorated prompt) ------------------------------------
    std::string git_project_;   // cwd basename
    std::string git_branch_;    // current branch, empty if not a git repo
    int git_ins_ = 0;           // git diff --shortstat insertions
    int git_del_ = 0;           // git diff --shortstat deletions
    void git_refresh();         // query git, populate fields above



    // A modal dialog (info_dialog / menu_select / config / session browser)
    // blocks the main thread in wgetch, so drain_events() cannot run. If the
    // agent needs an approval while a modal is open we queue it and resolve it
    // once the modal closes, instead of nesting ncurses dialogs or deadlocking
    // the worker on its promise.
    bool modal_open_ = false;
    std::queue<AgentEvent> pending_approvals_;
    // ---- geometry / layout ----------------------------------------------
    int height() const;
    int width() const;
    int chat_top() const;
    int chat_height() const;
    int lines_per_page() const;
    int stream_lines() const;
    int max_scroll() const;

    // ---- low-level helpers ----------------------------------------------
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
    void draw_input(const std::string& s, size_t cursor = 0, const std::string& shadow = "");
    void draw_drawer(const std::string& input);

    // ---- command drawer -------------------------------------------------

    static std::string drawer_token(const std::string& input);
    static bool drawer_has_arg(const std::string& input);
    std::vector<const tui::Command*> filter_commands(const std::string& token);

    // ---- streaming helpers ----------------------------------------------
    void fold_reasoning();
    void flush_stream();

    // ---- session persistence --------------------------------------------
    agent::Session snapshot(Window& w) const;
    void autosave();
    void save_session();
    void load_session(const std::string& id);
    void session_browser();


    // ---- window management ----------------------------------------------
    void switch_to(size_t idx);
    void lazy_load_active();
    void close_window();
    Window& win();
    const Window& win() const;

    // ---- slash command framework ----------------------------------------
    const std::vector<tui::Command>& commands();
    void build_commands();
    const tui::Command* find_command(const std::string& name);
    ActionRegistry action_registry_;
    std::string plugin_state_name(agent::PluginState st) const;
    bool handle_slash(const std::string& line);
    // Action-driven dispatch: handlers register per JSON tree action path.
    void register_action(const std::string& action,
                         std::function<void(const std::string&)> handler);
    void register_builtin_actions();
    void cmd_set_detection_toggle(const std::string& key, const std::string& val);
    void cmd_set_subagent_parallel(const std::string& val);
    void cmd_set_subagent_max(const std::string& val);
    void cmd_get_subagent();
    void cmd_set_reasoning_effort(const std::string& val);
    void cmd_get_reasoning();
    void cmd_model_set(const std::string& arg);
    void cmd_provider_list();    void cmd_provider_delete(const std::string& name);
    void cmd_provider_test(const std::string& name);
    void cmd_session_load(const std::string& id);
    void cmd_session_delete(const std::string& id);
    void cmd_files_ls(const std::string& rest);
    void cmd_files_tree(const std::string& rest);
    void cmd_files_open(const std::string& rest);
    void cmd_files_find(const std::string& rest);
    void cmd_system_exec(const std::string& rest);
    void cmd_system_delete(const std::string& rest);
    void cmd_system_rmdir(const std::string& rest);
    void cmd_system_mkdir(const std::string& rest);
    void cmd_system_mv(const std::string& rest);
    void cmd_system_cp(const std::string& rest);
    void cmd_system_info(const std::string& rest);
    void cmd_system_ps();
    void cmd_system_kill(const std::string& rest);
    void cmd_system_df();
    void cmd_system_uptime();
    void cmd_system_uname();
    void cmd_skills_interop(const std::string& val);
    void cmd_skills_refresh();
    void cmd_skills_create(const std::string& args);
    void cmd_skills_delete(const std::string& args);
    void cmd_skills_install(const std::string& source);
    void cmd_skills_uninstall(const std::string& name);
    void cmd_get_config();
    void cmd_get_model();
    void cmd_get_model_list();
    void cmd_get_model_context();
    void cmd_get_provider();
    void cmd_get_policy(const std::string& arg);
    void cmd_get_policy_rule(const std::string& arg);
    void cmd_set_policy_rule(const std::string& arg);
    void cmd_get_policy_mode();
    void cmd_get_policy_approval();
    void cmd_get_policy_timeout();
    void cmd_get_display();
    void cmd_get_think();
    void cmd_get_detection(const std::string& sub);
    void cmd_get_compression();
    void cmd_window_new();
    void cmd_window_close();
    void cmd_window_list();
    void cmd_window_rename(const std::string& name);
    void cmd_mcp_show(const std::string& server);
    void cmd_mcp_connect(const std::string& server);
    void cmd_mcp_disconnect(const std::string& server);
    void cmd_mcp_refresh(const std::string& server);
    void cmd_mcp_prompts(const std::string& server);
    void cmd_mcp_set_enabled(const std::string& server, bool on);
    void cmd_mcp_trust(const std::string& args);
    void cmd_plugin_list();
    void cmd_plugin_status(const std::string& id);
    void cmd_plugin_info(const std::string& id);
    void cmd_plugin_enable(const std::string& id);
    void cmd_plugin_disable(const std::string& id);
    void cmd_plugin_get(const std::string& args);
    void cmd_plugin_set(const std::string& args);
    void cmd_plugin_install(const std::string& source);
    void cmd_plugin_uninstall(const std::string& id);
    std::string usage(const tui::Command& c) const;
    void cmd_help(const std::string& arg);
    void cmd_window(const std::string& arg);
    void cmd_job(const std::string& rest);
    void cmd_compress(const std::string& arg);
    void cmd_set(const std::string& arg);
    void cmd_get(const std::string& arg);
    void cmd_skills_set(const std::string& rest);
    void cmd_skills_get(const std::string& sub);
    void cmd_mcp(const std::string& rest);
    void cmd_plugin(const std::string& rest);
    void refresh_completions();
    void cmd_prompt(const std::string& rest);
    void cmd_prompt_list();
    void cmd_provider(const std::string& arg);
    // Model picker data: cached /v1/models list with context info, refreshed
    // at startup and by /get model list. refresh_model_list() merges model
    // ids as value leaves under set.model (each with a generated action) so
    // the drawer, Tab completion, and dispatch all flow through the tree.
    std::vector<agent::ModelInfo> model_info_;
    void refresh_model_list();
    // Permission feed: tool names become value leaves under get/set
    // policy.rule (level + usage as help). Refreshed at startup and after
    // every rule mutation so the dangerous-command curation stays in sync.
    void refresh_policy_feed();
    void refresh_provider_feed();
    void apply_policy_rule(const std::string& name, const std::string& lvl);
    void show_policy_rule(const std::string& name);
    // Job feed: job ids become value leaves under job.kill / job.read.
    // Refreshed at startup and after every start/kill.
    void refresh_job_feed();
    void job_ls();
    void job_kill(const std::string& id);
    void job_read(const std::string& id);
    void job_start(const std::string& cmd);
    void request_quit();
    // Rejects configuration changes that would rebuild the agent's LLM
    // client while the worker thread is mid-request (use-after-free). Shows
    // a status line and returns true when the agent is busy.
    bool busy_reject(const std::string& what);
    // One restored tool call awaiting its result message (name + args from
    // the assistant message's tool_calls).
    struct RestoredCall {
        std::string name;
        agent::json args;
    };
    // Render one restored session message as scrollback lines. Assistant
    // tool_calls queue RestoredCall entries; tool messages emit a single
    // timestamped result line (describe + summary, no exit status).
    void restore_message_lines(const agent::Message& m,
                               std::vector<RestoredCall>& pending);
    // Effective compression threshold: the pipeline default when unset —
    // single source via load_compression_config (never a local magic number).
    double compression_threshold_effective() const {
        return agent::load_compression_config(cfg_).threshold;
    }
public:
    void save_workspace_now();
    void redraw_after_modal();

    void config_screen() const;
    void detect_server(bool force);
    bool test_connection(bool announce);
    void settings_screen();
    void send(const std::string& prompt);

    // ---- member variables -----------------------------------------------
    agent::Config cfg_;
    std::unique_ptr<agent::ProviderService> providers_;
    agent::ToolRegistry& reg_;
    agent::JobService& jobs_;
    agent::SubAgentExecutor& subagents_;       // host-owned; shared with process_* tools
    agent::PluginManager& plugins_; // host-owned; plugin lifecycle + tools
    agent::ServerManager mcp_servers_;  // session-scoped MCP manager
    std::string input_fill_;            // /prompt result applied to the input line
    agent::SessionStore store_;
    std::string settings_path_;

    std::vector<std::unique_ptr<Window>> windows_;
    size_t active_ = 0;

    Canvas chat_canvas_;                 // dedicated chat scrollback window
    md::Style md_style_;                 // markdown color mapping

    std::vector<tui::Command> commands_;
    tui::SettingRegistry settings_;
    void build_settings();
    bool quit_ = false;

    bool drawer_open_ = false;
    int drawer_sel_ = 0;
    bool show_reasoning_ = true;

    int policy_timeout_ = 60;
    agent::RunState state_ = agent::RunState::Idle;
    agent::Stats stats_;
    long ctx_used_ = -1;
    long live_ctx_offset_ = 0;   // running token count during streaming
    agent::ServerInfo last_detected_;
    int anim_phase_ = 0;
    // Turn start of the global "working" indicator (history-row spinner +
    // elapsed seconds); set when the agent becomes busy, cleared on idle.
    std::chrono::steady_clock::time_point working_since_{};
    // Whether the working row is currently shown: true while busy and no
    // output is being displayed yet; cleared on the first stream token and
    // re-set while a tool runs.
    bool working_visible_ = false;
    bool dirty_ = true;          // coalesce redraws into one flush per tick
    // Wall-clock timestamp of the last status-bar repaint, so the clock and
    // progress wave keep ticking while the agent is blocked on a call that
    // emits no streaming tokens.
    std::chrono::steady_clock::time_point last_status_tick_{};
    // Scroll-mode focus: when active, Up/Down/PgUp/PgDn scroll the chat window
    // instead of navigating the command line. Toggle with Escape.
    bool scroll_mode_ = false;
};

} // namespace tui

#endif

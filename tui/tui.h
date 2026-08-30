#ifndef AMBER_TUI_TUI_H
#define AMBER_TUI_TUI_H

#include <agent.h>
#include <agent/learn_commands.h>
#include <agent/mcp_config.h>
#include <agent/plugin.h>
#include <agent/plugin_registry.h>

#include "widgets.h"
#include "textutil.h"
#include "window.h"
#include "palette.h"
#include "rich.h"
#include "setting_registry.h"
#include "agent_event.h"
#include "event_router.h"
#include "window_manager.h"
#include "render_engine.h"
#include "session_controller.h"
#include "slash_dispatcher.h"

#include <atomic>
#include <chrono>
#include <functional>
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
class FeedManager;
class WindowManager;
class EventRouter;
class RenderEngine;
class SessionController;
class SlashDispatcher;

 // ncurses-based interactive TUI. Operates an IRC-style multi-window chat
// interface on top of the agent core. One instance per process; the main
// function creates it and calls run().
class Tui {
    friend class FeedManager;
    friend class WindowManager;
    friend class EventRouter;
    friend class RenderEngine;
    friend class SessionController;
    friend class SlashDispatcher;
public:
    Tui(agent::Config cfg, agent::ToolRegistry& reg, agent::JobService& jobs,
        agent::SubAgentExecutor& subagents, agent::PluginManager& plugins,
        agent::PluginRegistry& plugin_reg);
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    Window& new_window(const std::string& title);
    Window& open_welcome_window();
    Window& ensure_chat_window();
    void run();

    void save_workspace_now();
    void redraw_after_modal();
    void config_screen() const;
    void detect_server(bool force);
    bool test_connection(bool announce);
    void settings_screen();
    void send(const std::string& prompt);

private:
    // ---- event machinery (owned by EventRouter) --------------------------
    bool drain_events();
    void send_async(const std::string& raw_prompt);
    std::string expand_at_references(const std::string& raw) const;
    void agent_worker(Window& my_win, size_t window_id,
                      const std::string& prompt);
    void compress_worker(Window& my_win, size_t window_id);
    AgentEvent run_compression(Window& my_win, size_t window_id);
    std::unique_ptr<EventRouter> router_;
    std::string running_tool_;
    std::string running_tool_desc_;
    bool modal_open_ = false;
    std::string pending_prompt_;

    // ---- window management (owned by WindowManager) ---------------------
    void switch_to(size_t idx);
    void lazy_load_active();
    void close_window();
    Window& win();
    const Window& win() const;
    Window* window_by_id(size_t id);
    std::unique_ptr<WindowManager> window_manager_;

    // ---- scrollback helpers (central hub; render/session/events use) ----
    static size_t utf8_len(const std::string& s, size_t i);
    static std::vector<std::string> wrap_text(const std::string& text, int w);
    static std::string timestamp();
    void append_line(int color, const std::string& text);
    void append_line_ts(int color, const std::string& text,
                        const std::string& ts);
    size_t append_line_to(Window& w, int color, const std::string& text);
    size_t append_line_to(Window& w, int color, const std::string& text,
                          const std::string& ts);
    void append_rich(const rich::Line& l);
    void append_markdown(Window& w, const std::string& md);
    void append_rich_to(Window& w, const rich::Line& l);
    void banner(const std::string& text);
    void trim_lines(Window& w);
    void fold_reasoning(Window& w);
    void flush_stream(Window& w);
    void flush() { doupdate(); }

    // ---- rendering (owned by RenderEngine) -------------------------------
    void draw();
    void draw_status_bar(const std::string& tail);
    void tick_clock();
    void draw_input(const std::string& s, size_t cursor = 0, const std::string& shadow = "");
    void draw_drawer(const std::string& input);
    void git_refresh();
    std::unique_ptr<RenderEngine> render_engine_;

    // ---- session persistence (owned by SessionController) ----------------
    void autosave();
    void autosave(Window& w);
    void save_window_sessions();
    void load_session(const std::string& id);
    std::unique_ptr<SessionController> session_controller_;

    // ---- slash command framework (owned by SlashDispatcher) --------------
    const std::vector<tui::Command>& commands();
    void build_commands();
    bool handle_slash(const std::string& line);
    void register_action(const std::string& action,
                         std::function<void(const std::string&)> handler);
    void register_builtin_actions();
    void refresh_completions();
    void refresh_model_list();
    void refresh_policy_feed();
    void refresh_provider_feed();
    void refresh_job_feed();
    bool busy_reject(const std::string& what);
    void request_quit();
    void cmd_model_set(const std::string& arg);
    void cmd_provider(const std::string& arg);
    void job_kill(const std::string& id);
    void job_read(const std::string& id);
    void apply_policy_rule(const std::string& name, const std::string& lvl);
    void show_policy_rule(const std::string& name);
    std::unique_ptr<SlashDispatcher> slash_dispatcher_;

    // ---- member variables -----------------------------------------------
    agent::Config cfg_;
    std::unique_ptr<agent::ProviderService> providers_;
    agent::ToolRegistry& reg_;
    agent::JobService& jobs_;
    agent::SubAgentExecutor& subagents_;       // host-owned; shared with process_* tools
    agent::PluginManager& plugins_; // host-owned; plugin lifecycle + tools
    agent::PluginRegistry& plugin_reg_;  // v2 plugin registry
    agent::Workspace workspace_; // workspace instance for PluginContext
    std::unique_ptr<agent::PluginContext> plugin_ctx_; // owned context for v2 plugins
    std::unique_ptr<FeedManager> feed_manager_; // feed leaves for completions
    agent::ServerManager mcp_servers_;  // session-scoped MCP manager
    std::string input_fill_;            // /prompt result applied to the input line
    tui::SettingRegistry settings_;
    void build_settings();
    bool quit_ = false;

    agent::RunState state_ = agent::RunState::Idle;
    agent::Stats stats_;
    long ctx_used_ = -1;
    long live_ctx_offset_ = 0;   // running token count during streaming
    agent::ServerInfo last_detected_;
    int policy_timeout_ = 60;
};

} // namespace tui

#endif
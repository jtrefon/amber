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
#include "tui_ui_services.h"
#include "event_router.h"
#include "run_registry.h"
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
class TuiWindowOpsHooks;
class WindowOps;
class KeyBinder;

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
    friend class TuiWindowOpsHooks;

public:
    Tui(agent::Config cfg, agent::ToolRegistry& reg, agent::JobService& jobs,
        agent::SubAgentExecutor& subagents, agent::PluginManager& plugins,
        agent::PluginRuntime& plugin_runtime);
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    Window& new_window(const std::string& title);
    Window& open_welcome_window();
    Window& ensure_chat_window();
    void run();

    void redraw_after_modal();
    void config_screen() const;
    void detect_server(bool force);
    void test_connection(bool announce);
    void settings_screen();
    void send(const std::string& prompt);

    // Prompt for an API key for the active provider (UI thread, secret
    // field), persist it to the provider's config file, and return the
    // entered key — or "" when the user cancels. Called from
    // EventRouter::resolve_api_key in response to an AgentHooks::on_api_key
    // request (401/403 or keyless provider switch).
    std::string prompt_api_key(const std::string& reason);

    // ---- plugin user-interaction port (§8) -------------------------------
    // The port's TUI implementation needs three things from the host: a way to
    // block a plugin's thread on a question answered by the UI thread, a way to
    // show a notification, and a way to schedule work onto the UI thread.
    // These are that surface, kept on the host side of the interface.
    AskAnswer request_ask(const std::shared_ptr<AgentEvent>& ev);
    void notify_from_plugin(agent::UiLevel level, const std::string& message);
    void post_to_ui_thread(std::function<void()> work);
    // Run whatever plugins scheduled on the UI thread. Called once per tick,
    // before the bar renders, so a posted snapshot is what the segment reads.
    void drain_ui_posts();

private:
    // ---- event machinery (owned by EventRouter) --------------------------
    bool drain_events();
    void send_async(const std::string& raw_prompt);
    // Dispatch (or queue) a prompt on a specific window's run slot.
    void send_async_to(Window& w, const std::string& raw_prompt);
    // Dequeue prompts on windows whose agents finished (UI thread, per tick).
    void drain_pending_prompts();
    // Cancel every window's run: per-agent tokens + per-slot flags.
    void cancel_all_runs();
    std::string expand_at_references(const std::string& raw) const;
    void agent_worker(Window& my_win, size_t window_id, RunSlot* slot, const std::string& prompt);
    void compress_worker(Window& my_win, size_t window_id);
    AgentEvent run_compression(Window& my_win, size_t window_id);
    std::unique_ptr<EventRouter> router_;
    // Posted work from plugins and detached catalog workers, drained on the
    // tick like the other queues. Heap-held and shared: a worker whose result
    // lands after ~Tui still touches a live mutex/queue and its post is
    // dropped on the closed gate — never a torn member.
    struct UiPostQueue {
        std::mutex mtx;
        std::vector<std::function<void()>> queue;
        std::atomic<bool> alive{true};
    };
    std::shared_ptr<UiPostQueue> ui_posts_ = std::make_shared<UiPostQueue>();
    // The port itself, handed to the runtime so plugins can ask the user.
    std::unique_ptr<TuiUiServices> ui_services_;
    bool modal_open_ = false;

    // Per-window run slots: each window's agent has its own worker thread,
    // busy/cancel flags, and pending-prompt queue (see run_registry.h).
    RunRegistry runs_;

    // ---- window management (owned by WindowManager) ---------------------
    void switch_to(size_t idx);
    void lazy_load_active();
    void close_window();
    Window& win();
    const Window& win() const;
    Window* window_by_id(size_t id);
    std::unique_ptr<WindowManager> window_manager_;
    std::unique_ptr<TuiWindowOpsHooks> window_ops_hooks_;
    std::unique_ptr<WindowOps> window_ops_;
    std::unique_ptr<KeyBinder> key_binder_;

    // ---- scrollback helpers (central hub; render/session/events use) ----
    static size_t utf8_len(const std::string& s, size_t i);
    static std::vector<std::string> wrap_text(const std::string& text, int w);
    static std::string timestamp();
    void append_line(int color, const std::string& text);
    void append_line_ts(int color, const std::string& text, const std::string& ts);
    size_t append_line_to(Window& w, int color, const std::string& text);
    size_t append_line_to(Window& w, int color, const std::string& text, const std::string& ts);
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
    void draw_input(const std::string& s, size_t cursor = 0, const std::string& shadow = "");
    std::unique_ptr<RenderEngine> render_engine_;

    // ---- session persistence (owned by SessionController) ----------------
    void autosave();
    void autosave(Window& w);
    void load_session(const std::string& id);
    std::unique_ptr<SessionController> session_controller_;

    // ---- slash command framework (owned by SlashDispatcher) --------------
    const std::vector<tui::Command>& commands();
    bool handle_slash(const std::string& line);
    void refresh_completions();
    void register_action(const std::string& action,
                         std::function<void(const std::string&)> handler);
    bool busy_reject(const std::string& what);
    void refresh_model_list();
    // Cache-first model catalog: detection and forced probes never block the
    // UI thread. The catalog revalidates on a detached worker (single-flight
    // in the core); the result lands here via the ui_posts_ queue.
    void refresh_models_async(bool announce);
    void on_models_refreshed(bool fetched, bool announce, const std::string& api_base,
                             const std::string& flavor);
    // A post_to_ui_thread variant for background catalog work: the post is
    // dropped once destruction begins, so a late worker can never touch a
    // torn-down Tui.
    std::function<void(std::function<void()>)> ui_poster();
    void refresh_policy_feed();
    void refresh_provider_feed();
    void refresh_job_feed();
    void refresh_plugin_feed();
    void cmd_model_set(const std::string& arg);
    void cmd_provider(const std::string& arg);
    void show_plugin(const std::string& id);
    void report_toolset_audit();
    void set_plugin(const std::string& id, bool on);
    // Open the contributed-panel view (Alt+0 or /panel). Empty id = the first
    // registered panel, which is always the registry console.
    void open_panels(const std::string& id);
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
    agent::SubAgentExecutor& subagents_;        // host-owned; shared with process_* tools
    agent::PluginManager& plugins_;             // host-owned; v1 external plugin lifecycle
    agent::PluginRuntime& plugin_runtime_;      // v2 runtime (registries + ledger)
    std::unique_ptr<FeedManager> feed_manager_; // feed leaves for completions
    agent::ServerManager mcp_servers_;          // session-scoped MCP manager
    std::string input_fill_;                    // /prompt result applied to the input line
    tui::SettingRegistry settings_;
    void build_settings();
    bool quit_ = false;

    agent::ServerInfo last_detected_;
    int policy_timeout_ = 60;
    std::atomic<bool> models_refresh_inflight_{false};
};

} // namespace tui

#endif
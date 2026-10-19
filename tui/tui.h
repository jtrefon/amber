#ifndef AMBER_TUI_TUI_H
#define AMBER_TUI_TUI_H

#include <agent.h>
#include <agent/learn_commands.h>
#include <agent/plugin.h>
#include <agent/plugin_registry.h>
#include <agent/session.h>

#include "widgets.h"
#include "textutil.h"
#include "window.h"
#include "palette.h"
#include "rich.h"
#include "setting_registry.h"
#include "agent_event.h"
#include "command_line.h"
#include "key_binder.h"
#include "tui_ui_services.h"
#include "event_router.h"
#include "run_registry.h"
#include "window_manager.h"
#include "render_engine.h"
#include "session_controller.h"
#include "slash_dispatcher.h"
#include "model_picker.h"

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

namespace detail {

struct UiPostQueue {
    std::mutex mtx;
    std::vector<std::function<void()>> queue;
    std::atomic<bool> alive{true};
};

struct WatchedJob {
    std::shared_ptr<agent::Job> job;
    std::string label;
};

enum class PromptOutcome {
    NotOurs,  // no layer claimed the key: just flush what is pending
    Consumed, // already handled and redrawn
    Routed,   // CommandLine produced a result to act on
};

} // namespace detail

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

    // Open a chat window. prepare_workspace gives it its own git worktree when
    // the project is a repository and `worktree` is on, named by
    // `workspace_name` (or after the window). Preparation never blocks the
    // window: a failure opens it on the project root and says why.
    Window& new_window(const std::string& title, bool prepare_workspace = true,
                       const std::string& workspace_name = "");
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

    // Run a shell command through the job service and report its output when it
    // finishes. The UI thread starts the job and returns; a per-tick drain
    // reports the result. (The UI thread never polls or forks.)
    void run_command_async(const std::string& label, const std::string& cmd);
    void drain_pending_jobs();
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
    std::shared_ptr<detail::UiPostQueue> ui_posts_ = std::make_shared<detail::UiPostQueue>();
    // The port itself, handed to the runtime so plugins can ask the user.
    std::unique_ptr<TuiUiServices> ui_services_;
    bool modal_open_ = false;
    // Mouse reporting is on by default (wheel scrolls the chat log). Off lets
    // the terminal select and copy with a plain drag.
    bool mouse_on_ = true;

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
    size_t append_rich_to(Window& w, const rich::Line& l);
    void banner(const std::string& text);
    void trim_lines(Window& w);
    void fold_reasoning(Window& w);
    void flush_stream(Window& w);
    void flush();

    // ---- rendering (owned by RenderEngine) -------------------------------
    void draw();
    void draw_input(const std::string& s, size_t cursor = 0, const std::string& shadow = "");
    std::unique_ptr<RenderEngine> render_engine_;

    // Commands started by the UI and not yet reported. Job handles are
    // shared_ptrs, so a job that outlives the drain is still safe to read.
    std::vector<detail::WatchedJob> watched_jobs_;

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
    // Fetch a catalogue for EVERY configured provider, not just the active one.
    //
    // /set model lists the union of the configured providers' catalogues, and a
    // catalogue only exists on disk after a fetch. refresh_models_async() covers
    // cfg_ alone, so a provider that had never been selected contributed nothing
    // and looked absent -- which is exactly what configuring openrouter then
    // opening /set model showed.
    //
    // Deliberately separate from refresh_models_async(): that one is single-flight
    // for the ACTIVE endpoint and its result is applied to cfg_ (endpoint, model
    // and context autodetect). This one only needs the union to grow, so its
    // completion re-merges the feed and does not touch cfg_. model_catalog_
    // refresh_async does its own single-flight per endpoint, so several providers
    // fetch concurrently.
    void refresh_provider_catalogs_async();
    // /get provider list: print every provider as a pending row and settle each
    // one IN PLACE when its probe answers, so the list is one line per provider
    // and its colour reflects an answer that was just fetched.
    void show_provider_list();
    void settle_provider_row(unsigned request, const std::string& name, agent::AuthState state);
    void on_models_refreshed(const agent::CatalogFetchReport& refresh, bool announce,
                             const std::string& api_base, const std::string& flavor);
    void adopt_detected_model();
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
    // Mouse reporting off/on (Alt+M). Off returns the terminal's own click-drag
    // selection — copy without Shift — at the cost of wheel scrolling, which is
    // the trade-off the mouse was turned on for in the first place.
    void toggle_mouse();
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
    std::string input_fill_;                    // /prompt result applied to the input line
    tui::SettingRegistry settings_;
    void build_settings();
    // Provider management (the /settings screen): build the picker rows, add a
    // provider, and act on the selected one. settings_screen() orchestrates.
    void build_provider_picker(std::vector<std::string>& rows, std::vector<std::string>& ids) const;
    void add_new_provider();
    void provider_actions(const std::string& selected_id);
    void activate_and_edit_provider(const std::string& id, const agent::Provider* sel,
                                    bool is_preset);
    bool quit_ = false;
    // A workspace release happens while the closing window still exists (the
    // erase follows), so its message waits here for the next redraw instead of
    // being written into a window that is about to disappear.
    std::string pending_workspace_notice_;

    // --- Construction. The constructor is a facade over these phases, which
    // --- must stay in this order (the runtime needs the live config before it
    // --- starts, and the managers exist before the workspace is restored).
    void init_terminal();
    void wire_plugin_runtime();
    void build_managers();
    void load_key_bindings();
    void restore_workspace();

    // ---- per-window workspaces -------------------------------------------
    // A window's workspace is a git worktree of the project, kept under
    // $TMPDIR/amber-wt: volatile by design, because uncommitted work is all it
    // holds. See docs/fix-proposal/window-worktrees-2026-10-16.md.
    // Give the window a freshly prepared worktree (never throws; reports by
    // line, and leaves the window on the project root on any failure).
    void prepare_window_workspace(Window& w, const std::string& requested_name);
    // Remove the window's workspace when that can lose nothing; otherwise keep
    // it and say why. The notice is flushed after the window is erased.
    void release_window_workspace(Window& w);
    void flush_workspace_notice();
    // The tree the ACTIVE window's tools, prompts and status bar refer to: its
    // workspace when it has one, else the project root.
    std::string active_workspace_root() const;
    // Adopt a restored window's recorded workspace. String-level only: the
    // first paint must never wait on a subprocess, so anything that needs git
    // (reaping, rebuilding a wiped workspace) is queued for start_workspace_pass.
    void adopt_recorded_workspace(Window& w, const agent::WorkspaceState::WindowEntry& entry);
    // The git half of the workspace lifecycle, off the UI thread: reap what no
    // window claims, rebuild workspaces whose directory is gone, and record
    // where each live workspace stopped. Results are posted back.
    void start_workspace_pass();
    // Apply what the pass found, on the UI thread.
    void apply_workspace_result(const std::vector<std::pair<size_t, std::string>>& rebuilt,
                                const std::vector<std::string>& notices);
    struct PendingRebuild {
        size_t window_id = 0;
        std::string name;
        std::string base;
        std::string path;
    };
    std::vector<PendingRebuild> pending_rebuild_;

    // --- Event loop. run() is a facade over these steps, one per thing the
    // --- loop does: start up, drain, dispatch a key, act on the result.
    void run_startup(CommandLine& cl);
    void refresh_completion_context(CommandLine& cl);
    [[noreturn]] void shutdown_from_signal();
    bool pump_io(CommandLine& cl);
    void idle_tick(bool had_events, CommandLine& cl);
    bool handle_option_key(int& ch, CommandLine& cl);
    bool handle_key_binding(int ch, CommandLine& cl);
    bool apply_key_action(const KeyAction& act, int ch, CommandLine& cl);
    bool cancel_active_run(int ch, CommandLine& cl);
    bool handle_ctrl_c(int ch, CommandLine& cl);
    bool handle_mouse_wheel(int ch, CommandLine& cl);
    bool scroll_mode_nav(int ch);
    detail::PromptOutcome route_to_command_line(int ch, CommandLine& cl,
                                                CommandLine::Result& result);
    void run_prompt_action(const CommandLine::Result& result, CommandLine& cl);
    void dispatch_prompt(const std::string& text, CommandLine& cl);
    void show_prompt_popup(CommandLine& cl);
    void pick_reference_file(CommandLine& cl);
    void pick_command(CommandLine& cl);
    void show_prompt_help(const CommandLine::Result& result, CommandLine& cl);
    void flush_if_dirty();

    agent::ServerInfo last_detected_;
    std::uint64_t tool_registry_generation_ = 0; // the set the tree reflects
    int policy_timeout_ = 60;
    std::atomic<bool> models_refresh_inflight_{false};
    // Set by /get model list when the catalogue is cold: the refresh it kicks
    // may fail, and the user who asked must hear WHY rather than a "still
    // loading" that never resolves.
    bool report_model_failure_ = false;
    // Rows of /get provider list still awaiting their probe, and the id of the
    // invocation that printed them (two runs of the command coexist, and a late
    // answer must settle its own row, not the newer one).
    PendingProviderRows provider_rows_;
    unsigned provider_list_seq_ = 0;
};

} // namespace tui

#endif
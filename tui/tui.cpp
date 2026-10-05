// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "command_line.h"
#include "confirm_panel.h"
#include "tool_display.h"
#include "scroll_dispatch.h"
#include "keys_ncurses.h"
#include "help_page.h"
#include "completion_context.h"
#include "option_key_decode.h"
#include "signal_guard.h"
#include "event_router.h"
#include "feed_manager.h"
#include "path_confine.h"
#include "panel_view.h"
#include "tui_window_ops_hooks.h"
#include "window_ops.h"
#include "key_binder.h"

#include <agent.h>
#include <agent/mcp_tools.h>
#include <agent/compressor.h>
#include <agent/experience.h>
#include <agent/tools.h>
#include <agent/data_path.h>
#include <agent/plugin.h>

#include <array>
#include <cstdio>
#include <memory>

#include "widgets.h"
#include "textutil.h"
#include "welcome.h"

#include <unistd.h>

#include <clocale>
#include <csignal>
#include <ctime>
#include <functional>

namespace tui {

namespace {
// Process-global signal state. The handler may only touch async-signal-safe
// machinery: set the flag, restore the terminal, arm the alarm fallback. The
// main event loop turns the flag into a graceful teardown (workspace save +
// endwin); if the loop cannot run (e.g. blocked in a modal), SIGALRM kills the
// process two seconds later with the terminal already restored.
SignalState g_signal_state;
TerminalGuard g_terminal_guard;

// An '@' reference token ends at whitespace or punctuation.
bool is_ref_terminator(char c) {
    return c == ' ' || c == '\t' || c == ',' || c == '.' || c == '!' || c == '?' || c == ';' ||
           c == ':';
}

// The expansion of one '@' token: the file's contents in a marker block, or the
// token unchanged when it does not name a readable in-workspace file.
std::string reference_expansion(const std::string& ref) {
    std::string resolved, err;
    if (!tui::confine_path(ref, resolved, err))
        return ref;
    namespace fs = std::filesystem;
    const fs::path ref_path(resolved);
    std::error_code ec;
    if (!fs::is_regular_file(ref_path, ec))
        return ref;
    std::ifstream f(ref_path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.size() > 4096)
        content.resize(4096);
    return "\n[file: " + ref + "]\n" + content + "\n[/file]\n";
}
} // namespace

// Restores the terminal and hands control back to the main loop for a
// graceful exit. No file I/O, no allocations, no locks — only async-signal-
// safe calls. If the loop never consumes the flag (modal block), the SIGALRM
// fallback terminates the process.
static void signal_handler(int sig) {
    g_signal_state.raise(sig);
    g_terminal_guard.restore();
    alarm(2);
}

// The signal handler restores the terminal and defers the workspace save to the
// main loop (a save from inside a signal handler is async-signal-unsafe).
// SIGALRM is left at its default: the handler's alarm(2) is the fallback death
// when the main loop cannot run.
void install_signal_handlers() {
    std::signal(SIGHUP, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

Tui::Tui(agent::Config cfg, agent::ToolRegistry& reg, agent::JobService& jobs,
         agent::SubAgentExecutor& subagents, agent::PluginManager& plugins,
         agent::PluginRuntime& plugin_runtime)
    : cfg_(std::move(cfg)), providers_(agent::make_default_provider_service(cfg_)), reg_(reg),
      jobs_(jobs), subagents_(subagents), plugins_(plugins), plugin_runtime_(plugin_runtime),
      mcp_servers_(agent::load_mcp_servers(), &this->cfg_.cancel_token) {
    init_terminal();
    install_signal_handlers();
    wire_plugin_runtime();
    build_managers();
    load_key_bindings();
    register_mcp_tools();
    restore_workspace();
}

void Tui::init_terminal() {
    std::setlocale(LC_ALL, "");
    g_terminal_guard.capture();
    initscr();
    raw(); // capture Ctrl-C as keypress (ASCII 3) instead of SIGINT
    noecho();
    keypad(stdscr, TRUE);
    meta(stdscr, TRUE); // receive the high-bit (0xB1..0xB9) Alt+number form
    set_escdelay(25);
    curs_set(1);
    start_color();
    // Mouse wheel events (BUTTON4/5) scroll the chat log. Enabling mouse
    // reporting replaces terminal-level click-drag selection — the trade-off
    // documented in docs/architecture/scroll-design.md — but keeps wheel
    // ticks distinct from arrow keys, so Up/Down stay prompt-history
    // navigation instead of the xterm alt-scroll aliasing.
    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
    mouseinterval(0);
    set_modal_flag(&modal_open_);
    use_default_colors();
    use_legacy_coding(1);
    init_pairs();
}

// The runtime needs the LIVE config: the Tui owns cfg_ by value, so a plugin
// reading an API key or the active provider must be pointed at it rather than at
// the copy the runtime took at construction. The port is attached before
// activation, so a plugin that asks something during initialize() gets a real
// answer rather than the fail-closed null; activation then happens with the live
// configuration in place, so a plugin reading it sees the real thing from its
// first call.
void Tui::wire_plugin_runtime() {
    plugin_runtime_.attach_config(cfg_);
    ui_services_ = std::make_unique<TuiUiServices>(
        [this](const std::shared_ptr<AgentEvent>& ev) { return request_ask(ev); },
        [this](agent::UiLevel level, const std::string& msg) { notify_from_plugin(level, msg); },
        [this](std::function<void()> work) { post_to_ui_thread(std::move(work)); });
    plugin_runtime_.attach_ui_services(ui_services_.get());
    plugin_runtime_.start();
    feed_manager_ = std::make_unique<FeedManager>(*this);
}

void Tui::build_managers() {
    window_manager_ = std::make_unique<WindowManager>(cfg_, reg_, &plugin_runtime_);
    router_ = std::make_unique<EventRouter>(*this);
    render_engine_ = std::make_unique<RenderEngine>(*this);
    session_controller_ = std::make_unique<SessionController>(*this);
    slash_dispatcher_ = std::make_unique<SlashDispatcher>(*this);
    window_ops_hooks_ = std::make_unique<TuiWindowOpsHooks>(*this);
    window_ops_ = std::make_unique<WindowOps>(*window_manager_, *window_ops_hooks_);
}

// Bindings come from keybindings.json, resolved the same way as
// completions.json: a bare relative path silently leaves every binding empty
// when amber runs outside the source tree or from an install prefix.
void Tui::load_key_bindings() {
    const std::string exe = agent::exe_path();
    nlohmann::json kj;
    for (const auto& c :
         agent::data_file_candidates("keybindings.json", exe.empty() ? nullptr : exe.c_str())) {
        std::ifstream kf(c);
        if (kf.is_open()) {
            kf >> kj;
            break;
        }
    }
    key_binder_ = std::make_unique<KeyBinder>(std::move(kj));
}

void Tui::register_mcp_tools() {
    reg_.register_tool(agent::make_read_resource_tool(mcp_servers_));
    mcp_servers_.connect_all();
    for (const auto& st : mcp_servers_.snapshot())
        if (st.connected)
            agent::register_server_tools(reg_, mcp_servers_, st.name);
}

// Restore the previous workspace: open saved sessions in their own windows. On
// first launch (no saved workspace) show the welcome mural instead.
void Tui::restore_workspace() {
    auto ws = session_controller_->load_workspace();
    if (ws.windows.empty()) {
        open_welcome_window();
        return;
    }
    for (const auto& we : ws.windows) {
        Window& w = new_window(we.title.empty() ? "chat" : we.title);
        w.session_id = we.session_id;
        w.prompt_history = we.prompt_history;
        w.history_pos = w.prompt_history.size();
    }
    if (ws.active < window_manager_->count())
        window_manager_->set_active(ws.active);
    lazy_load_active();
}

Tui::~Tui() {
    // Detached catalog workers post results through ui_poster(); closing the
    // gate under the queue lock guarantees no post can interleave with the
    // member teardown below. The queue itself is shared, so a late worker
    // still lands on live memory.
    {
        std::scoped_lock lk(ui_posts_->mtx);
        ui_posts_->alive = false;
    }
    cancel_all_runs();
    {
        std::scoped_lock lk(router_->mutex());
        router_->set_shutting_down(true);
        deny_all_pending_approvals(router_->queue());
        deny_all_pending_approvals(router_->pending_approvals());
        deny_all_pending_api_keys(router_->pending_api_keys());
    }
    runs_.join_all();
    endwin();
    session_controller_->save_window_sessions();
    session_controller_->save_workspace_now();
}

Window& Tui::new_window(const std::string& title) {
    return window_manager_->new_window(title);
}

Window& Tui::open_welcome_window() {
    return window_manager_->open_welcome_window();
}

Window& Tui::ensure_chat_window() {
    return window_manager_->ensure_chat_window();
}

Window& Tui::win() {
    return window_manager_->win();
}
const Window& Tui::win() const {
    return window_manager_->win();
}

// ---- thread / event machinery -------------------------------------------

std::string Tui::expand_at_references(const std::string& raw) const {
    std::string out;
    size_t i = 0;
    while (i < raw.size()) {
        const size_t at = raw.find('@', i);
        if (at == std::string::npos || at == 0) {
            out += raw.substr(i);
            break;
        }
        out += raw.substr(i, at - i);
        // The reference token runs to the next space or punctuation.
        size_t end = at + 1;
        while (end < raw.size() && !is_ref_terminator(raw[end]))
            ++end;
        const std::string ref = raw.substr(at + 1, end - at - 1);
        if (!ref.empty())
            out += reference_expansion(ref);
        i = end;
    }
    return out;
}

void Tui::send_async(const std::string& raw_prompt) {
    send_async_to(ensure_chat_window(), raw_prompt);
}

void Tui::send_async_to(Window& w, const std::string& raw_prompt) {
    // Per-window gate: a busy window queues its own prompt; sibling windows
    // dispatch immediately — "agent is running" is a per-window fact.
    if (runs_.busy(w.id)) {
        runs_.enqueue(w.id, raw_prompt);
        append_line_to(w, P_STATUS, "queued");
        return;
    }
    runs_.join(w.id); // harvest the previous worker
    runs_.clear_cancel(w.id);
    RunSlot* slot = &runs_.slot(w.id);
    slot->busy = true;
    render_engine_->mark_working();

    append_line_to(w, P_USER, "> " + raw_prompt);
    w.reason.begin();
    render_engine_->set_show_reasoning(cfg_.show_reasoning);
    w.stream_ts = timestamp();

    std::string prompt = expand_at_references(raw_prompt);

    // Capture the window on the UI thread: the worker must never read
    // window_manager_->all()/window_manager_->active() (the UI thread mutates them) — it gets its
    // own Window* and stamps every event with the window's stable id so drain_events can route even
    // after other windows close.
    Window* my_win = &w;
    size_t my_id = w.id;
    // The worker captures its RunSlot* — slot addresses are stable, and the
    // busy-flag write must not re-enter the registry map (erase() joins
    // under the map lock, so a map lookup from the worker would deadlock).
    slot->thread = std::thread(
        [this, my_win, my_id, slot, prompt] { agent_worker(*my_win, my_id, slot, prompt); });
}

// Dispatch prompts queued while their window's agent was busy. Runs on the
// UI thread each idle tick; a window only dequeues when its own slot is
// free, so queued prompts never jump the line onto a running window.
void Tui::drain_pending_prompts() {
    for (auto& w : window_manager_->all()) {
        if (!w || !w->agent || runs_.busy(w->id))
            continue;
        auto p = runs_.pop_pending(w->id);
        if (p)
            send_async_to(*w, *p);
    }
}

void Tui::run_command_async(const std::string& label, const std::string& cmd) {
    // The job service owns execution: timeout-bounded, output-capped, visible in
    // /jobs and killable. This returns immediately — the UI thread must never
    // wait on a subprocess.
    std::string id = jobs_.start(cmd, agent::Workspace::root(), 60, 30);
    if (id.empty()) {
        append_line(P_STATUS, label + ": failed to start: " + cmd);
        return;
    }
    append_line(P_STATUS, label + ": started " + id + " \u2014 output when it finishes");
    if (auto job = jobs_.get(id))
        watched_jobs_.push_back({std::move(job), label});
    draw();
}

void Tui::drain_pending_jobs() {
    for (auto it = watched_jobs_.begin(); it != watched_jobs_.end();) {
        if (!it->job || !it->job->is_done()) {
            ++it;
            continue;
        }
        // Job::output() is incremental, so the first read after completion is
        // the whole transcript.
        std::string out = it->job->output();
        if (out.size() > 4096)
            out.resize(4096);
        append_line(P_STATUS, it->label + ": exit " + std::to_string(it->job->exit_code()));
        if (!out.empty())
            append_line(P_ASSISTANT, out);
        draw(); // the result is on screen, not merely in the scrollback
        it = watched_jobs_.erase(it);
    }
}

void Tui::cancel_all_runs() {
    // Two channels per window: the agent's own token aborts its run loop
    // and in-flight cancellable tools (RunScope); the slot flag drops its
    // event stream and skips queued dispatch.
    for (auto& w : window_manager_->all()) {
        if (!w)
            continue;
        if (w->agent)
            w->agent->request_cancel();
        runs_.request_cancel(w->id);
    }
}

void Tui::agent_worker(Window& my_win, size_t window_id, RunSlot* slot, const std::string& prompt) {
    agent::AgentHooks hooks = router_->make_hooks(window_id, slot->cancel);

    try {
        if (!slot->cancel.load()) {
            my_win.agent->set_hooks(hooks);
            my_win.agent->run(prompt);
            my_win.dirty = true;
        }
    } catch (const std::exception& e) {
        AgentEvent ev;
        ev.type = AgentEvent::Error;
        ev.window_id = window_id;
        ev.error_msg = e.what();
        router_->push(std::move(ev));
    } catch (...) {
        AgentEvent ev;
        ev.type = AgentEvent::Error;
        ev.window_id = window_id;
        ev.error_msg = "agent thread terminated unexpectedly";
        router_->push(std::move(ev));
    }

    AgentEvent done;
    done.type = AgentEvent::Done;
    done.window_id = window_id;
    router_->push(std::move(done));
    slot->busy = false;
}

void Tui::compress_worker(Window& my_win, size_t window_id) {
    if (runs_.busy(window_id))
        return; // callers check; belt & braces
    runs_.join(window_id);
    RunSlot* slot = &runs_.slot(window_id);
    slot->busy = true;
    my_win.compressing = true;
    render_engine_->mark_working();
    slot->thread = std::thread([this, my_win = &my_win, window_id, slot]() {
        AgentEvent ev = run_compression(*my_win, window_id);
        router_->push(std::move(ev));
        slot->busy = false;
    });
}

AgentEvent Tui::run_compression(Window& my_win, size_t window_id) {
    AgentEvent ev;
    ev.type = AgentEvent::CompressResult;
    ev.window_id = window_id;
    try {
        if (my_win.agent)
            ev.compress_result = my_win.agent->compress_now();
    } catch (const std::exception& e) {
        // Same degradation as agent_worker: an exception must never escape a
        // std::thread entry (std::terminate) and the result must still be
        // reported so state_ does not stay Waiting.
        ev.type = AgentEvent::Error;
        ev.error_msg = e.what();
    } catch (...) {
        ev.type = AgentEvent::Error;
        ev.error_msg = "compress thread terminated unexpectedly";
    }
    return ev;
}

// =========================================================================
// Event loop. run() is a facade; each step below owns one thing the loop does.
// =========================================================================

void Tui::run() {
    CommandLine cl;
    run_startup(cl);

    while (!quit_) {
        // Deferred signal handling: the handler only set a flag (and restored
        // the terminal). Turn it into a graceful save + teardown here, on the
        // main thread, where file I/O and endwin are safe. Never returns.
        if (g_signal_state.consume())
            shutdown_from_signal();

        const bool had_events = pump_io(cl);

        int ch = getch();
        if (ch == ERR) {
            idle_tick(had_events, cl);
            continue;
        }

        if (handle_option_key(ch, cl))
            continue;

        if (handle_key_binding(ch, cl))
            continue;
        if (handle_ctrl_c(ch, cl))
            continue;
        if (handle_mouse_wheel(ch, cl))
            continue;

        CommandLine::Result result;
        switch (route_to_command_line(ch, cl, result)) {
        case detail::PromptOutcome::Consumed:
            break;
        case detail::PromptOutcome::Routed:
            run_prompt_action(result, cl);
            break;
        case detail::PromptOutcome::NotOurs:
            flush_if_dirty();
            break;
        }
    }
}

// First paint, config, and the command tree. The registry and tree are built
// FIRST: refresh_completions rebuilds the tree from completions.json and then
// merges the live feeds (models, providers, policy rules, jobs, plugin states),
// and merging feeds before the rebuild wiped their leaves from the tree.
void Tui::run_startup(CommandLine& cl) {
    render_engine_->request_git_refresh(); // worker-side: never blocks the first paint
    render_engine_->draw();
    render_engine_->draw_input("");
    render_engine_->flush();
    detect_server(false);
    timeout(kTickTimeoutMs);

    build_settings();
    (void)commands(); // force command tree build
    // Load completion metadata from JSON (help text, choices, ranges). This is
    // the single source of truth for completion metadata — code edits cannot
    // break completion unless the JSON file is damaged.
    refresh_completions();

    // CommandLine is pure logic (no ncurses) and fully tested via e2e tests.
    cl.set_history(win().prompt_history);
    refresh_completion_context(cl);
}

// Completion context from the command tree. This is the SINGLE source of
// completions — no duplicate logic in draw_drawer (see completion_context).
void Tui::refresh_completion_context(CommandLine& cl) {
    completion_context::Context ctx = completion_context::for_input(cl.text(), settings_);
    cl.set_completions(ctx.rows, ctx.prefix);
}

// Graceful teardown after a signal, on the main thread. Never returns: the
// process exits with the shell-conventional 128+signal status.
[[noreturn]] void Tui::shutdown_from_signal() {
    // Persist per-window conversation sessions. snapshot() races the worker, so
    // only save when it has finished (agent_busy_ is cleared as its last
    // action); a mid-run signal exits without the final turn's session —
    // bounded shutdown beats a torn save.
    cancel_all_runs();
    {
        std::scoped_lock lk(router_->mutex());
        router_->set_shutting_down(true);
        deny_all_pending_approvals(router_->queue());
        deny_all_pending_approvals(router_->pending_approvals());
        deny_all_pending_api_keys(router_->pending_api_keys());
    }
    // endwin() first: the session-save progress writes to stderr and must not
    // interleave with a terminal ncurses still controls.
    endwin();
    // Workers observe their per-agent tokens and exit promptly; a truly wedged
    // worker (blocked read) may outlive us — bounded shutdown still beats a torn
    // save, so only idle windows save.
    runs_.join_all();
    for (auto& w : window_manager_->all())
        if (w && w->agent && !runs_.busy(w->id))
            session_controller_->autosave(*w);
    session_controller_->save_workspace_now();
    _Exit(128 + g_signal_state.signal());
}

// Drain everything the workers posted, then apply the tick. Returns true when
// any event arrived, which decides how the idle branch repaints.
bool Tui::pump_io(CommandLine& cl) {
    const bool had_events = drain_events();
    drain_ui_posts();
    jobs_.check_timeouts();
    drain_pending_jobs();
    // Time-driven plugin work (a provider's balance refresh, say). The bar reads
    // what the tick cached; segments themselves never fetch.
    plugin_runtime_.tick();
    if (!input_fill_.empty()) {
        cl.set_text(input_fill_);
        input_fill_.clear();
    }
    return had_events;
}

// No key this tick: repaint what changed, and keep the clock and the tool
// spinners alive.
void Tui::idle_tick(bool had_events, CommandLine& cl) {
    if (had_events) {
        draw();
    } else {
        auto now = std::chrono::steady_clock::now();
        if (now - render_engine_->last_status_tick() > std::chrono::milliseconds(150)) {
            render_engine_->set_last_status_tick(now);
            router_->advance_tool_spinners();
            if (runs_.any_busy())
                render_engine_->draw();
            else
                render_engine_->tick_clock();
        }
    }
    render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
    drain_pending_prompts();
    if (render_engine_->dirty())
        render_engine_->flush();
}

// macOS Option-as-text form: terminals with default Option settings send the
// literal Option characters as UTF-8 (Option+1 = '¡' U+00A1, ...) instead of an
// ESC prefix. Assemble the sequence and normalize digit-row glyphs to the
// meta-digit path so Alt+number works with no terminal configuration. Other
// Option glyphs stay non-insertable, matching the prior drop of non-ASCII
// input. Returns true when the key was consumed here; `ch` is rewritten to the
// meta-digit code when one was assembled, and the caller carries on with it.
bool Tui::handle_option_key(int& ch, CommandLine& cl) {
    if (!option_key_decode::is_lead(static_cast<unsigned char>(ch)))
        return false;
    const int need = option_key_decode::continuation_bytes(static_cast<unsigned char>(ch));
    unsigned char seq[4] = {static_cast<unsigned char>(ch)};
    int got = 0;
    timeout(50);
    for (; got < need; ++got) {
        int b = getch();
        if (b == ERR)
            break;
        if ((b & 0xC0) != 0x80) {
            ungetch(b); // not a continuation byte — don't eat the key
            break;
        }
        seq[got + 1] = static_cast<unsigned char>(b);
    }
    timeout(kTickTimeoutMs);
    if (got != need)
        return true; // partial sequence: consumed, nothing to act on

    const uint32_t cp = option_key_decode::codepoint(seq, need);
    if (int d = macos_option_digit(cp); d >= 0) {
        ch = 0xB0 + d; // the meta-digit continues down the normal key path
        return false;
    }
    if (cp == kMacosOptionB) {
        cl.on_ctrl_w();
        draw_input(cl.text(), cl.cursor(), cl.shadow());
        return true;
    }
    return true; // other Option glyphs stay dropped
}

// KeyBinder dispatch for window hotkeys (Alt+1..9, Ctrl+N, ESC+digit, ESC
// stateful). The KeyBinder is pure (no ncurses); the ESC followup read is
// terminal I/O and stays here.
bool Tui::handle_key_binding(int ch, CommandLine& cl) {
    // The keys the binder owns: Alt+0..9, Ctrl+N, ESC, Ctrl+C, Ctrl+W. Alt+0 used to
    // be special-cased in run() and excluded here, which meant the panel view had two
    // sources of truth -- a binding for some keys, a branch in the loop for others.
    const bool binder_owns =
        (ch >= 0xB0 && ch <= 0xB9) || ch == 14 || ch == 27 || ch == 3 || ch == 23;
    if (!binder_owns)
        return false;

    InputState state;
    state.drawer_open = cl.drawer_open();
    // "busy" to the key layer means THE ACTIVE window's agent is running — it
    // drives ESC=cancel semantics only, never gating.
    state.busy = runs_.busy(win().id);
    state.scroll_mode = render_engine_->scroll_mode();
    state.window_count = window_manager_->count();
    state.has_pending_prompt = runs_.has_pending(win().id);

    KeyRead kr{ch, std::nullopt};
    if (ch == 27) {
        timeout(200);
        int n = getch();
        timeout(kTickTimeoutMs);
        if (n != ERR)
            kr.followup = n;
    }

    return apply_key_action(key_binder_->dispatch(kr, state), ch, cl);
}

// Act on what the binder decided. Returns true when the key was consumed.
bool Tui::apply_key_action(const KeyAction& act, int ch, CommandLine& cl) {
    switch (act.type) {
    case KeyAction::SwitchWindow:
        switch_to(static_cast<size_t>(act.arg));
        break;
    case KeyAction::NewWindow:
        new_window("chat");
        render_engine_->draw();
        break;
    case KeyAction::CloseDrawer:
        render_engine_->draw();
        break;
    case KeyAction::DeleteWord:
        cl.on_ctrl_w();
        break;
    case KeyAction::CancelOrQuit:
        return cancel_active_run(ch, cl);
    case KeyAction::ToggleScrollMode:
        render_engine_->set_scroll_mode(!render_engine_->scroll_mode());
        if (render_engine_->scroll_mode())
            append_line(P_STATUS, "scroll mode — arrows/PgUp/PgDn navigate window");
        render_engine_->draw();
        break;
    case KeyAction::OpenPanels:
        // The panel view (registry console, status). Panels own their content; the host
        // only opens them. Alt+0 reaches here as an intent rather than as a branch in
        // run(), so the next panel entry point is additive.
        open_panels("");
        render_engine_->draw();
        break;
    case KeyAction::None:
    default:
        // No binding for this key: fall through to the later layers (ESC handling, then
        // CommandLine routing). This used to special-case ESC+0 to open panels, but
        // dispatch_esc never returns None for it -- it returns ToggleScrollMode -- so
        // the branch was unreachable and the comment above it described a behaviour the
        // code did not have.
        return false;
    }
    // Every action this layer handles repaints the input line; it used to be
    // repeated in each case.
    draw_input(cl.text(), cl.cursor(), cl.shadow());
    return true;
}


// Ctrl+C cancels the active window's run; ESC only reaches this action when the
// binder saw the ACTIVE window busy, so it always cancels. Returns false when
// the key should fall through to the quit path instead.
// Repaint the chat log and the input line together. The most common render step in
// the input loop, and it was written out at five call sites with two different
// spellings (Tui::draw_input vs render_engine_->draw_input), so a change to how the
// input line is drawn had to be found rather than made.
void Tui::redraw(const CommandLine& cl) {
    render_engine_->draw();
    draw_input(cl.text(), cl.cursor(), cl.shadow());
}

bool Tui::cancel_active_run(int ch, CommandLine& cl) {
    if (ch == 3 && !runs_.busy(win().id))
        return false; // idle Ctrl+C: the quit path handles it
    // Cancels the ACTIVE window's run only: the slot flag drops its event stream
    // and the agent token aborts its loop and any in-flight cancellable tool
    // (via RunScope).
    if (win().agent)
        win().agent->request_cancel();
    runs_.request_cancel(win().id);
    append_line(P_STATUS, "cancelling…");
    redraw(cl);
    return true;
}

// Ctrl+C on an idle window: quit. When sibling windows are still running,
// quitting needs a confirm — the destructor then cancels and joins every
// worker. Returns true when the key was handled.
bool Tui::handle_ctrl_c(int ch, CommandLine& cl) {
    if (ch != 3)
        return false;
    if (runs_.busy(win().id)) {
        if (win().agent)
            win().agent->request_cancel();
        runs_.request_cancel(win().id);
        append_line(P_STATUS, "cancelling…");
        redraw(cl);
        return true;
    }
    if (runs_.any_busy()) {
        ConfirmPanel q("Quit", std::to_string(runs_.busy_count()) +
                                   " agent(s) still running — quit anyway?");
        if (!q.run()) {
            redraw_after_modal();
            redraw(cl);
            return true;
        }
    }
    session_controller_->save_workspace_now();
    quit_ = true;
    return true;
}

// Mouse wheel: scroll the chat log (never prompt history). Wheel ticks arrive
// as KEY_MOUSE (alt-scroll is off), so this is the only place they can land;
// Up/Down keys keep their meaning.
bool Tui::handle_mouse_wheel(int ch, CommandLine& cl) {
    if (ch != KEY_MOUSE)
        return false;
    MEVENT ev;
    if (getmouse(&ev) == OK) {
        int delta = scroll_dispatch::wheel_delta(ev.bstate);
        if (delta != 0) {
            win().scroll_top = scroll_dispatch::clamped_scroll_top(
                win().scroll_top, delta, render_engine_->max_scroll(win()));
            redraw(cl);
        }
    }
    return true;
}

// In scroll mode the arrows and page keys move the window instead of the
// prompt. Returns true when the key was consumed here; the caller still runs the
// normal redraw path.
bool Tui::scroll_mode_nav(int ch) {
    if (!render_engine_->scroll_mode())
        return false;
    switch (ch) {
    case KEY_UP:
        win().scroll_top = std::max(0, win().scroll_top - 1);
        break;
    case KEY_DOWN:
        win().scroll_top += 1;
        break;
    case KEY_PPAGE:
        win().scroll_top = std::max(0, win().scroll_top - 10);
        break;
    case KEY_NPAGE:
        win().scroll_top = std::min(render_engine_->max_scroll(), win().scroll_top + 10);
        break;
    default:
        return false;
    }
    render_engine_->draw();
    return true;
}

// One entry per editing key: everything that needs more than a plain
// CommandLine call is handled around this table.
struct EditKey {
    int key;
    CommandLine::Result (CommandLine::*handler)();
};

const EditKey kEditKeys[] = {
    {'\t', &CommandLine::on_tab},        {KEY_UP, &CommandLine::on_up},
    {KEY_DOWN, &CommandLine::on_down},   {KEY_LEFT, &CommandLine::on_left},
    {KEY_RIGHT, &CommandLine::on_right}, {KEY_HOME, &CommandLine::on_home},
    {KEY_END, &CommandLine::on_end},     {KEY_BACKSPACE, &CommandLine::on_backspace},
    {127, &CommandLine::on_backspace},   {8, &CommandLine::on_backspace},
    {1, &CommandLine::on_ctrl_a},        {5, &CommandLine::on_ctrl_e},
    {11, &CommandLine::on_ctrl_k},       {20, &CommandLine::on_ctrl_t},
    {21, &CommandLine::on_ctrl_u},       {23, &CommandLine::on_ctrl_w},
    {25, &CommandLine::on_ctrl_y},       {31, &CommandLine::on_undo},
    {4, &CommandLine::on_ctrl_d},
};

// Route a key to CommandLine (pure logic, unit tested).
detail::PromptOutcome Tui::route_to_command_line(int ch, CommandLine& cl,
                                                 CommandLine::Result& result) {
    if (scroll_mode_nav(ch))
        return detail::PromptOutcome::Routed; // redrawn; no CommandLine action

    for (const EditKey& b : kEditKeys) {
        if (b.key == ch) {
            result = (cl.*b.handler)();
            return detail::PromptOutcome::Routed;
        }
    }

    if (ch == 10 || ch == 13 || ch == KEY_ENTER) {
        if (render_engine_->scroll_mode()) {
            render_engine_->set_scroll_mode(false);
            render_engine_->draw();
        }
        result = cl.on_enter();
        return detail::PromptOutcome::Routed;
    }

    if (ch == 18) {
        append_line(P_STATUS, "Ctrl-R: not yet implemented");
        render_engine_->draw();
        render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
        return detail::PromptOutcome::Consumed;
    }

    if (ch >= 32 && ch <= 126) {
        result = cl.on_char(static_cast<char>(ch));
        return detail::PromptOutcome::Routed;
    }
    return detail::PromptOutcome::NotOurs;
}

// CommandLine produced a result: sync the drawer, then act on it.
void Tui::run_prompt_action(const CommandLine::Result& result, CommandLine& cl) {
    // Update completion context for shadow computation.
    refresh_completion_context(cl);
    // CommandLine owns the drawer logic now; mirror its state into the renderer.
    render_engine_->set_drawer_open(cl.drawer_open());
    render_engine_->set_drawer_sel(cl.drawer_sel());
    switch (result.action) {
    case CommandLine::Result::Dispatch:
        dispatch_prompt(result.dispatch_text, cl);
        return;
    case CommandLine::Result::ShowPopup:
        show_prompt_popup(cl);
        return;
    case CommandLine::Result::ShowHelpPage:
        show_prompt_help(result, cl);
        return;
    default:
        break;
    }
    render_engine_->draw();
    render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
    flush_if_dirty();
}

// Remember the prompt, then run it: a slash command, or an agent turn.
void Tui::dispatch_prompt(const std::string& text, CommandLine& cl) {
    auto& ph = win().prompt_history;
    if (!text.empty() && (ph.empty() || ph.back() != text)) {
        ph.push_back(text);
        if (ph.size() > 100)
            ph.erase(ph.begin());
    }
    win().history_pos = ph.size();
    cl.set_history(ph);
    if (!handle_slash(text))
        send_async(text);
    render_engine_->draw();
    render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
}

// '@' picks a workspace file; anything else picks a command from the palette.
void Tui::show_prompt_popup(CommandLine& cl) {
    if (!cl.text().empty() && cl.text().back() == '@')
        pick_reference_file(cl);
    else
        pick_command(cl);
    render_engine_->draw();
    render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
}

void Tui::pick_reference_file(CommandLine& cl) {
    namespace fs = std::filesystem;
    const std::string root = agent::Workspace::root();
    std::vector<std::string> items;
    for (const auto& e : fs::directory_iterator(root)) {
        std::string name = e.path().filename().string();
        if (name.front() == '.')
            continue;
        if (fs::is_directory(e))
            name += "/";
        items.push_back(name);
    }
    std::sort(items.begin(), items.end());
    if (items.empty())
        return;
    const int sel = menu_select("reference file:", items);
    if (sel < 0 || sel >= static_cast<int>(items.size()))
        return;
    std::string ref = items[sel];
    if (ref.back() == '/')
        ref.pop_back();
    cl.set_text_and_cursor(cl.text() + ref, cl.text().size() + ref.size());
}

void Tui::pick_command(CommandLine& cl) {
    const std::string tok = palette::token(cl.text());
    auto matches = render_engine_->filter_commands(tok);
    if (matches.empty())
        return;
    std::vector<std::string> items;
    items.reserve(matches.size());
    for (auto* c : matches)
        items.emplace_back(palette::usage(*c) + "  " + c->help);
    menu_select("options:", items);
}

// '?' opens the man page for the node; leaves fall back to a one-line status,
// then to cmd_help for top-level commands.
void Tui::show_prompt_help(const CommandLine::Result& result, CommandLine& cl) {
    const std::string help_key = help_page::key_from_node(result.help_node);
    std::vector<std::string> page = help_page::build(settings_, help_key);
    if (!page.empty()) {
        info_dialog(help_key, page);
        redraw_after_modal();
        redraw(cl);
        return;
    }
    const std::string msg = help_page::fallback_line(settings_, help_key);
    if (!msg.empty()) {
        append_line(P_STATUS, msg);
        redraw(cl);
        return;
    }
    slash_dispatcher_->cmd_help(help_page::command_from_node(result.help_node));
    redraw(cl);
}

void Tui::flush_if_dirty() {
    if (render_engine_->dirty()) {
        render_engine_->flush();
        render_engine_->clear_dirty();
    }
}

void Tui::redraw_after_modal() {
    session_controller_->redraw_after_modal();
}
void Tui::autosave() {
    session_controller_->autosave();
}
void Tui::autosave(Window& w) {
    session_controller_->autosave(w);
}
void Tui::load_session(const std::string& id) {
    session_controller_->load_session(id);
}
void Tui::flush() {
    doupdate();
}

void Tui::draw() {
    render_engine_->draw();
}
void Tui::draw_input(const std::string& s, size_t cursor, const std::string& shadow) {
    render_engine_->draw_input(s, cursor, shadow);
}
void Tui::build_settings() {
    slash_dispatcher_->build_settings();
}
bool Tui::drain_events() {
    return router_->drain_events();
}
const std::vector<tui::Command>& Tui::commands() {
    return slash_dispatcher_->commands();
}
bool Tui::handle_slash(const std::string& line) {
    return slash_dispatcher_->handle_slash(line);
}
void Tui::register_action(const std::string& action,
                          std::function<void(const std::string&)> handler) {
    slash_dispatcher_->register_action(action, std::move(handler));
}
bool Tui::busy_reject(const std::string& what) {
    return slash_dispatcher_->busy_reject(what);
}
void Tui::refresh_completions() {
    slash_dispatcher_->refresh_completions();
}
void Tui::refresh_model_list() {
    slash_dispatcher_->refresh_model_list();
}
void Tui::refresh_policy_feed() {
    slash_dispatcher_->refresh_policy_feed();
}
void Tui::refresh_provider_feed() {
    slash_dispatcher_->refresh_provider_feed();
}
void Tui::refresh_job_feed() {
    slash_dispatcher_->refresh_job_feed();
}
void Tui::refresh_plugin_feed() {
    if (feed_manager_)
        feed_manager_->refresh_plugin_feed();
}
void Tui::cmd_model_set(const std::string& arg) {
    slash_dispatcher_->cmd_model_set(arg);
}
void Tui::cmd_provider(const std::string& arg) {
    slash_dispatcher_->cmd_provider(arg);
}
void Tui::show_plugin(const std::string& id) {
    slash_dispatcher_->show_plugin(id);
}
void Tui::report_toolset_audit() {
    slash_dispatcher_->report_toolset_audit();
}
void Tui::set_plugin(const std::string& id, bool on) {
    slash_dispatcher_->set_plugin(id, on);
}
void Tui::open_panels(const std::string& id) {
    const std::string shown = panel_view(plugin_runtime_.panels(), id);
    if (shown.empty()) {
        append_line(P_STATUS, "no panels registered");
        return;
    }
    // The panel view owns the screen while it is up; repaint around it.
    redraw_after_modal();
}
// A plugin's question: post it to the same queue the worker-side asks use, then
// block until the UI thread answers. Returning early while shutting down is the
// difference between a stopped shutdown and a hung one.
AskAnswer Tui::request_ask(const std::shared_ptr<AgentEvent>& ev) {
    // Before the router exists (activation) or after it stops (shutdown) there
    // is no one to answer: fail closed rather than block forever.
    if (!router_ || router_->shutting_down())
        return {};
    std::shared_future<AskAnswer> answer = ev->ask_promise->get_future().share();
    router_->push(*ev);
    return answer.get();
}

void Tui::notify_from_plugin(agent::UiLevel level, const std::string& message) {
    const int color = level == agent::UiLevel::Info ? P_STATUS : P_USER;
    const std::string line = level == agent::UiLevel::Info
                                 ? message
                                 : std::string(agent::to_string(level)) + ": " + message;
    post_to_ui_thread([this, color, line] { append_line(color, line); });
}

void Tui::post_to_ui_thread(std::function<void()> work) {
    std::scoped_lock lk(ui_posts_->mtx);
    ui_posts_->queue.push_back(std::move(work));
}

std::function<void(std::function<void()>)> Tui::ui_poster() {
    return [posts = ui_posts_](std::function<void()> work) {
        std::scoped_lock lk(posts->mtx);
        if (posts->alive)
            posts->queue.push_back(std::move(work));
    };
}

void Tui::drain_ui_posts() {
    std::vector<std::function<void()>> work;
    {
        std::scoped_lock lk(ui_posts_->mtx);
        if (ui_posts_->queue.empty())
            return;
        work.swap(ui_posts_->queue);
    }
    for (auto& fn : work) {
        try {
            fn();
        } catch (...) {
            // A plugin's posted callable is plugin code: a throw is a lost
            // repaint, never a dead UI thread.
        }
    }
}

void Tui::job_kill(const std::string& id) {
    slash_dispatcher_->job_kill(id);
}
void Tui::job_read(const std::string& id) {
    slash_dispatcher_->job_read(id);
}
void Tui::apply_policy_rule(const std::string& name, const std::string& lvl) {
    slash_dispatcher_->apply_policy_rule(name, lvl);
}
void Tui::show_policy_rule(const std::string& name) {
    slash_dispatcher_->show_policy_rule(name);
}

Window* Tui::window_by_id(size_t id) {
    return find_window(window_manager_->all(), id);
}

} // namespace tui
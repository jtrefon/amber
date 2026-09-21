// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "command_line.h"
#include "confirm_panel.h"
#include "drawer_rows.h"
#include "tool_display.h"
#include "scroll_dispatch.h"
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

Tui::Tui(agent::Config cfg, agent::ToolRegistry& reg, agent::JobService& jobs,
         agent::SubAgentExecutor& subagents, agent::PluginManager& plugins,
         agent::PluginRuntime& plugin_runtime)
    : cfg_(std::move(cfg)), providers_(agent::make_default_provider_service(cfg_)), reg_(reg),
      jobs_(jobs), subagents_(subagents), plugins_(plugins), plugin_runtime_(plugin_runtime),
      mcp_servers_(agent::load_mcp_servers(), &this->cfg_.cancel_token) {
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

    // Signal handler restores the terminal and defers the workspace save to
    // the main loop (a save from inside a signal handler is async-signal-
    // unsafe). SIGALRM is left at its default: the handler's alarm(2) is the
    // fallback death when the main loop cannot run.
    std::signal(SIGHUP, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // The runtime needs the LIVE config: the Tui owns cfg_ by value, so a
    // plugin reading an API key or the active provider must be pointed at it
    // rather than at the copy the runtime took at construction.
    plugin_runtime_.attach_config(cfg_);
    // The port is attached before activation, so a plugin that asks something
    // during initialize() gets a real answer rather than the fail-closed null.
    ui_services_ = std::make_unique<TuiUiServices>(
        [this](const std::shared_ptr<AgentEvent>& ev) { return request_ask(ev); },
        [this](agent::UiLevel level, const std::string& msg) { notify_from_plugin(level, msg); },
        [this](std::function<void()> work) { post_to_ui_thread(std::move(work)); });
    plugin_runtime_.attach_ui_services(ui_services_.get());
    // Activate now, with the live config in place: a plugin that reads the
    // configuration (a balance endpoint, an API key) must see the real thing
    // from its first call.
    plugin_runtime_.start();
    feed_manager_ = std::make_unique<FeedManager>(*this);
    window_manager_ = std::make_unique<WindowManager>(cfg_, reg_, &plugin_runtime_);
    router_ = std::make_unique<EventRouter>(*this);
    render_engine_ = std::make_unique<RenderEngine>(*this);
    session_controller_ = std::make_unique<SessionController>(*this);
    slash_dispatcher_ = std::make_unique<SlashDispatcher>(*this);
    window_ops_hooks_ = std::make_unique<TuiWindowOpsHooks>(*this);
    window_ops_ = std::make_unique<WindowOps>(*window_manager_, *window_ops_hooks_);
    {
        // Same data-path resolution as completions.json: a bare relative
        // path silently leaves every binding empty when amber runs outside
        // the source tree or from an install prefix.
        std::string exe = agent::exe_path();
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

    reg_.register_tool(agent::make_read_resource_tool(mcp_servers_));
    mcp_servers_.connect_all();
    for (const auto& st : mcp_servers_.snapshot())
        if (st.connected)
            agent::register_server_tools(reg_, mcp_servers_, st.name);

    // Restore previous workspace: open saved sessions in their own windows.
    // On first launch (no saved workspace) show the welcome mural instead.
    auto ws = session_controller_->load_workspace();
    if (!ws.windows.empty()) {
        for (const auto& we : ws.windows) {
            Window& w = new_window(we.title.empty() ? "chat" : we.title);
            w.session_id = we.session_id;
            w.prompt_history = we.prompt_history;
            w.history_pos = w.prompt_history.size();
        }
        if (ws.active < window_manager_->count())
            window_manager_->set_active(ws.active);
        lazy_load_active();
    } else {
        open_welcome_window();
    }
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
        size_t at = raw.find('@', i);
        if (at == std::string::npos || at == 0) {
            out += raw.substr(i);
            break;
        }
        out += raw.substr(i, at - i);
        // Find end of reference token (space, end, punctuation).
        size_t end = at + 1;
        while (end < raw.size() && raw[end] != ' ' && raw[end] != '\t' && raw[end] != ',' &&
               raw[end] != '.' && raw[end] != '!' && raw[end] != '?' && raw[end] != ';' &&
               raw[end] != ':')
            ++end;
        std::string ref = raw.substr(at + 1, end - at - 1);
        if (!ref.empty()) {
            namespace fs = std::filesystem;
            std::string resolved, err;
            if (!tui::confine_path(ref, resolved, err)) {
                out += ref;
            } else {
                fs::path ref_path(resolved);
                std::error_code ec;
                if (fs::is_regular_file(ref_path, ec)) {
                    std::ifstream f(ref_path);
                    std::string content((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
                    if (content.size() > 4096)
                        content.resize(4096);
                    out += "\n[file: " + ref + "]\n";
                    out += content;
                    out += "\n[/file]\n";
                } else {
                    out += ref;
                }
            }
        }
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

void Tui::run() {
    render_engine_->request_git_refresh(); // worker-side: never blocks the first paint
    render_engine_->draw();
    render_engine_->draw_input("");
    render_engine_->flush();
    detect_server(false);
    timeout(kTickTimeoutMs);

    // Build the setting registry and command tree FIRST; refresh_completions
    // rebuilds the tree from completions.json and then merges the live feeds
    // (models, providers, policy rules, jobs, plugin states). Merging feeds
    // before the rebuild wiped their leaves from the tree.
    build_settings();
    (void)commands(); // force command tree build
    // Load completion metadata from JSON (help text, choices, ranges).
    // This is the single source of truth for completion metadata — code edits
    // cannot break completion unless the JSON file is damaged.
    refresh_completions();

    // CommandLine is pure logic (no ncurses) and fully tested via e2e tests.
    CommandLine cl;
    cl.set_history(win().prompt_history);

    // Helper: update CommandLine's completion context from the command tree and JSON.
    // This is the SINGLE source of completions — no duplicate logic in draw_drawer.
    auto update_completions = [&]() {
        std::string input = cl.text();
        if (!input.empty() && input[0] == '/') {
            // The drawer is the visible contract: feed exactly its entry
            // names so arrow selection and Enter dispatch index the same
            // rows the user sees (aliases are not drawer rows).
            auto names = drawer_entry_names(input, settings_);
            // Dispatch prefix Enter prepends to the selected row:
            //   "/window"        (namespace descend) -> "/window "  (keep ns)
            //   "/c"             (partial)           -> "/"         (replace)
            //   "/set model "    (trailing space)    -> "/set model "
            std::string prefix;
            if (!input.empty() && input.back() == ' ') {
                prefix = input; // explicit descend
            } else {
                size_t tok_start = input.rfind(' ');
                tok_start = (tok_start == std::string::npos) ? 1 : tok_start + 1;
                std::string last_tok = input.substr(tok_start);
                // drawer_entry_names descends into a namespace when the
                // trailing token resolves to children (drawer_rows.cpp); in
                // that case the namespace is KEPT (prefix = input + " ").
                // Otherwise the token is a partial being typed and is
                // REPLACED (prefix = input minus the token).
                std::string ns_so_far = input.substr(1, tok_start - 1);
                while (!ns_so_far.empty() && ns_so_far.back() == ' ')
                    ns_so_far.pop_back();
                std::string probe = ns_so_far.empty() ? last_tok : ns_so_far + "." + last_tok;
                if (!settings_.children_of(probe).empty())
                    prefix = input + " ";
                else
                    prefix = input.substr(0, tok_start);
            }
            cl.set_completions(names, prefix);
            return;
        }
        // Non-slash text: top-level command names from the tree, including
        // JSON-declared aliases — never a hardcoded list.
        std::vector<std::string> names = settings_.complete("");
        for (const auto& a : settings_.top_level_aliases())
            names.push_back(a);
        cl.set_completions(names);
    };
    update_completions();

    while (!quit_) {
        // Deferred signal handling: the handler only set a flag (and restored
        // the terminal). Turn it into a graceful save + teardown here, on the
        // main thread, where file I/O and endwin are safe.
        if (g_signal_state.consume()) {
            // Persist per-window conversation sessions and exit with the
            // shell-conventional 128+sig status. snapshot() races the worker,
            // so only save when it has finished (agent_busy_ is cleared as
            // its last action); a mid-run signal exits without the final
            // turn's session — bounded shutdown beats a torn save.
            cancel_all_runs();
            {
                std::scoped_lock lk(router_->mutex());
                router_->set_shutting_down(true);
                deny_all_pending_approvals(router_->queue());
                deny_all_pending_approvals(router_->pending_approvals());
                deny_all_pending_api_keys(router_->pending_api_keys());
            }
            // endwin() first: the session-save progress writes to stderr and
            // must not interleave with a terminal ncurses still controls.
            endwin();
            // Workers observe their per-agent tokens and exit promptly; a
            // truly wedged worker (blocked read) may outlive us — bounded
            // shutdown still beats a torn save, so only idle windows save.
            runs_.join_all();
            for (auto& w : window_manager_->all())
                if (w && w->agent && !runs_.busy(w->id))
                    session_controller_->autosave(*w);
            session_controller_->save_workspace_now();
            _Exit(128 + g_signal_state.signal());
        }
        bool had_events = drain_events();
        drain_ui_posts();
        jobs_.check_timeouts();
        drain_pending_jobs();
        // Time-driven plugin work (a provider's balance refresh, say). The bar
        // reads what the tick cached; segments themselves never fetch.
        plugin_runtime_.tick();
        if (!input_fill_.empty()) {
            cl.set_text(input_fill_);
            input_fill_.clear();
        }

        int ch = getch();
        if (ch == ERR) {
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
            continue;
        }

        // macOS Option-as-text form: terminals with default Option settings
        // send the literal Option characters as UTF-8 (Option+1 = '¡'
        // U+00A1, ...) instead of an ESC prefix. Assemble the sequence and
        // normalize digit-row glyphs to the meta-digit path so Alt+number
        // works with no terminal configuration. Other Option glyphs stay
        // non-insertable, matching the prior drop of non-ASCII input.
        if (ch >= 0xC2 && ch <= 0xF4) {
            int need = (ch < 0xE0) ? 1 : (ch < 0xF0) ? 2 : 3;
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
                continue;
            uint32_t cp = seq[0] & ((need == 1) ? 0x1F : (need == 2) ? 0x0F : 0x07);
            for (int i = 1; i <= need; ++i)
                cp = (cp << 6) | (seq[i] & 0x3F);
            if (int d = macos_option_digit(cp); d >= 0)
                ch = 0xB0 + d;
            else if (cp == kMacosOptionB) {
                cl.on_ctrl_w();
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            } else
                continue;
        }

        // Alt+0 opens the panel view (the registry console first); the host
        // owns the key, the panels own their content.
        if (ch == 0xB0) {
            open_panels("");
            render_engine_->draw();
            draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }

        // KeyBinder dispatch for window hotkeys (Alt+1..9, Ctrl+N, ESC+digit,
        // ESC stateful). The KeyBinder is pure (no ncurses); the ESC followup
        // read is terminal I/O and stays here.
        if ((ch >= 0xB1 && ch <= 0xB9) || ch == 14 || ch == 27 || ch == 3 || ch == 23) {
            InputState state;
            state.drawer_open = cl.drawer_open();
            // "busy" to the key layer means THE ACTIVE window's agent is
            // running — it drives ESC=cancel semantics only, never gating.
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

            KeyAction act = key_binder_->dispatch(kr, state);

            switch (act.type) {
            case KeyAction::SwitchWindow:
                switch_to(static_cast<size_t>(act.arg));
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            case KeyAction::NewWindow:
                new_window("chat");
                render_engine_->draw();
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            case KeyAction::CloseDrawer:
                render_engine_->draw();
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            case KeyAction::DeleteWord:
                cl.on_ctrl_w();
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            case KeyAction::CancelOrQuit:
                // Ctrl+C on an idle window falls through to the quit path
                // below (cancel_or_quit). ESC only reaches this action when
                // the binder saw the ACTIVE window busy, so it never quits.
                if (ch == 3 && !runs_.busy(win().id))
                    break;
                // Cancels the ACTIVE window's run only: the slot flag drops
                // its event stream and the agent token aborts its loop and
                // any in-flight cancellable tool (via RunScope).
                if (win().agent)
                    win().agent->request_cancel();
                runs_.request_cancel(win().id);
                append_line(P_STATUS, "cancelling…");
                render_engine_->draw();
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            case KeyAction::ToggleScrollMode:
                render_engine_->set_scroll_mode(!render_engine_->scroll_mode());
                if (render_engine_->scroll_mode())
                    append_line(P_STATUS, "scroll mode — arrows/PgUp/PgDn navigate window");
                render_engine_->draw();
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            case KeyAction::None:
                // ESC+0 opens panels (not a window switch); fall through to
                // the CommandLine routing for other unhandled keys.
                if (ch == 27 && kr.followup && *kr.followup == '0') {
                    open_panels("");
                    render_engine_->draw();
                    draw_input(cl.text(), cl.cursor(), cl.shadow());
                    continue;
                }
                break;
            default:
                break;
            }
        }

        // Ctrl+C: cancel the active window's run, or save+exit. When
        // siblings are still running, quitting needs a confirm — the
        // destructor then cancels and joins every worker.
        if (ch == 3) {
            if (runs_.busy(win().id)) {
                if (win().agent)
                    win().agent->request_cancel();
                runs_.request_cancel(win().id);
                append_line(P_STATUS, "cancelling…");
                render_engine_->draw();
                render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            if (runs_.any_busy()) {
                ConfirmPanel q("Quit", std::to_string(runs_.busy_count()) +
                                           " agent(s) still running — quit anyway?");
                if (!q.run()) {
                    redraw_after_modal();
                    render_engine_->draw();
                    draw_input(cl.text(), cl.cursor(), cl.shadow());
                    continue;
                }
            }
            session_controller_->save_workspace_now();
            quit_ = true;
            break;
        }

        // Mouse wheel: scroll the chat log (never prompt history). Wheel
        // ticks arrive as KEY_MOUSE (alt-scroll is off), so this branch is
        // the only place they can land; Up/Down keys keep their meaning.
        if (ch == KEY_MOUSE) {
            MEVENT ev;
            if (getmouse(&ev) == OK) {
                int delta = scroll_dispatch::wheel_delta(ev.bstate);
                if (delta != 0) {
                    win().scroll_top = scroll_dispatch::clamped_scroll_top(
                        win().scroll_top, delta, render_engine_->max_scroll(win()));
                    render_engine_->draw();
                    render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                }
            }
            continue;
        }

        // ── Route through CommandLine (pure logic, unit tested) ─
        CommandLine::Result result;
        bool handled = true;

        switch (ch) {
        case '	':
            result = cl.on_tab();
            break;
        case KEY_UP:
            if (render_engine_->scroll_mode()) {
                win().scroll_top = std::max(0, win().scroll_top - 1);
                render_engine_->draw();
            } else
                result = cl.on_up();
            break;
        case KEY_DOWN:
            if (render_engine_->scroll_mode()) {
                win().scroll_top += 1;
                render_engine_->draw();
            } else
                result = cl.on_down();
            break;
        case KEY_LEFT:
            result = cl.on_left();
            break;
        case KEY_RIGHT:
            result = cl.on_right();
            break;
        case KEY_HOME:
            result = cl.on_home();
            break;
        case KEY_END:
            result = cl.on_end();
            break;
        case KEY_PPAGE:
            if (render_engine_->scroll_mode()) {
                win().scroll_top = std::max(0, win().scroll_top - 10);
                render_engine_->draw();
            }
            break;
        case KEY_NPAGE:
            if (render_engine_->scroll_mode()) {
                win().scroll_top = std::min(render_engine_->max_scroll(), win().scroll_top + 10);
                render_engine_->draw();
            }
            break;
        case KEY_BACKSPACE:
        case 127:
        case 8:
            result = cl.on_backspace();
            break;
        case 10:
        case 13:
        case KEY_ENTER:
            if (render_engine_->scroll_mode()) {
                render_engine_->set_scroll_mode(false);
                render_engine_->draw();
            }
            result = cl.on_enter();
            break;
        case 1:
            result = cl.on_ctrl_a();
            break;
        case 5:
            result = cl.on_ctrl_e();
            break;
        case 11:
            result = cl.on_ctrl_k();
            break;
        case 20:
            result = cl.on_ctrl_t();
            break;
        case 21:
            result = cl.on_ctrl_u();
            break;
        case 23:
            result = cl.on_ctrl_w();
            break;
        case 25:
            result = cl.on_ctrl_y();
            break;
        case 31:
            result = cl.on_undo();
            break;
        case 4:
            result = cl.on_ctrl_d();
            break;
        case 18:
            append_line(P_STATUS, "Ctrl-R: not yet implemented");
            render_engine_->draw();
            render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        default:
            if (ch >= 32 && ch <= 126)
                result = cl.on_char(static_cast<char>(ch));
            else
                handled = false;
            break;
        }

        if (handled) {
            // Update completion context for shadow computation.
            update_completions();
            // Sync drawer state from CommandLine (CommandLine owns drawer logic now).
            render_engine_->set_drawer_open(cl.drawer_open());
            render_engine_->set_drawer_sel(cl.drawer_sel());
            switch (result.action) {
            case CommandLine::Result::Dispatch: {
                std::string text = result.dispatch_text;
                auto& ph = win().prompt_history;
                if (!text.empty() && (ph.empty() || ph.back() != text)) {
                    ph.push_back(text);
                    if (ph.size() > 100)
                        ph.erase(ph.begin());
                }
                win().history_pos = ph.size();
                cl.set_history(ph);
                if (handle_slash(text)) {
                    render_engine_->draw();
                    render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                    continue;
                }
                send_async(text);
                render_engine_->draw();
                render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            case CommandLine::Result::ShowPopup: {
                if (!cl.text().empty() && cl.text().back() == '@') {
                    namespace fs = std::filesystem;
                    std::string root = agent::Workspace::root();
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
                    if (!items.empty()) {
                        int sel = menu_select("reference file:", items);
                        if (sel >= 0 && sel < static_cast<int>(items.size())) {
                            std::string ref = items[sel];
                            if (ref.back() == '/')
                                ref.pop_back();
                            cl.set_text_and_cursor(cl.text() + ref, cl.text().size() + ref.size());
                        }
                    }
                } else {
                    std::string tok = palette::token(cl.text());
                    auto matches = render_engine_->filter_commands(tok);
                    if (!matches.empty()) {
                        std::vector<std::string> items;
                        items.reserve(matches.size());
                        for (auto* c : matches)
                            items.emplace_back(palette::usage(*c) + "  " + c->help);
                        menu_select("options:", items);
                    }
                }
                render_engine_->draw();
                render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            case CommandLine::Result::ShowHelpPage: {
                std::string node = result.help_node;
                if (!node.empty() && node[0] == '/')
                    node = node.substr(1);
                std::string help_key = node;
                size_t first_sp = node.find(' ');
                if (first_sp != std::string::npos)
                    help_key = node.substr(first_sp + 1);
                // Try full man page first.
                std::string man = settings_.man_for(help_key);
                if (!man.empty()) {
                    std::vector<std::string> page;
                    // Header: help text as subtitle
                    std::string helptxt = settings_.help_for(help_key);
                    if (!helptxt.empty())
                        page.emplace_back(helptxt);
                    page.emplace_back("");
                    // Body: full man text with word wrapping
                    size_t pos = 0;
                    while (pos < man.size()) {
                        size_t next = man.find('\n', pos);
                        if (next == std::string::npos) {
                            page.emplace_back(man.substr(pos));
                            break;
                        }
                        page.emplace_back(man.substr(pos, next - pos));
                        pos = next + 1;
                    }
                    page.emplace_back("");
                    // Children listing
                    auto kids = settings_.children_of(help_key);
                    if (!kids.empty()) {
                        page.emplace_back("sub-commands:");
                        for (const auto& k : kids) {
                            std::string line = "  " + k;
                            std::string subkey = help_key;
                            subkey += ".";
                            subkey += k;
                            std::string h = settings_.help_for(subkey);
                            if (!h.empty()) {
                                line += "  —  ";
                                line += h;
                            }
                            page.emplace_back(line);
                        }
                        page.emplace_back("");
                    }
                    // Choices / range for leaf settings
                    const auto& ch_choices = settings_.choices_for(help_key);
                    if (!ch_choices.empty()) {
                        std::string line = "choices: ";
                        for (size_t i = 0; i < ch_choices.size(); ++i) {
                            if (i > 0)
                                line += ", ";
                            line += ch_choices[i];
                        }
                        page.push_back(line);
                    }
                    double rlo, rhi;
                    if (settings_.range_for(help_key, rlo, rhi))
                        page.push_back("range: " + std::to_string((int)rlo) + " – " +
                                       std::to_string((int)rhi));
                    info_dialog(help_key, page);
                    redraw_after_modal();
                    render_engine_->draw();
                    render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                    continue;
                }
                // Fallback to one-line status for leaf settings without man text.
                std::string desc = settings_.help_for(help_key);
                if (!desc.empty()) {
                    std::string msg = help_key;
                    msg += "  —  ";
                    msg += desc;
                    const auto& chc = settings_.choices_for(help_key);
                    if (!chc.empty()) {
                        msg += "  choices: ";
                        for (const auto& c : chc)
                            msg += c + "|";
                        msg.pop_back();
                    }
                    double rlo, rhi;
                    if (settings_.range_for(help_key, rlo, rhi))
                        msg += "  range: " + std::to_string(rlo) + "-" + std::to_string(rhi);
                    append_line(P_STATUS, msg);
                    render_engine_->draw();
                    render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                    continue;
                }
                // Fallback to cmd_help for top-level commands.
                size_t sp = node.find(' ');
                if (sp != std::string::npos)
                    node.resize(sp);
                slash_dispatcher_->cmd_help(node);
                render_engine_->draw();
                render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            default:
                break;
            }
            render_engine_->draw();
            render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
            if (render_engine_->dirty()) {
                render_engine_->flush();
                render_engine_->clear_dirty();
            }
            continue;
        }

        // Unhandled keys.
        if (ch == KEY_NPAGE) {
            win().scroll_top += render_engine_->lines_per_page();
            render_engine_->draw();
            render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }
        if (ch == KEY_PPAGE) {
            win().scroll_top = std::max(0, win().scroll_top - render_engine_->lines_per_page());
            render_engine_->draw();
            render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }
        if (render_engine_->dirty()) {
            render_engine_->flush();
            render_engine_->clear_dirty();
        }
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
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
    : cfg_(std::move(cfg)),
      providers_(agent::make_default_provider_service(cfg_)),
      reg_(reg), jobs_(jobs), subagents_(subagents),
      plugins_(plugins), plugin_runtime_(plugin_runtime),
      mcp_servers_(agent::load_mcp_servers(), &this->cfg_.cancel_token) {
    std::setlocale(LC_ALL, "");
    g_terminal_guard.capture();
    initscr();
    raw();        // capture Ctrl-C as keypress (ASCII 3) instead of SIGINT
    noecho();
    keypad(stdscr, TRUE);
    meta(stdscr, TRUE);   // receive the high-bit (0xB1..0xB9) Alt+number form
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

    reg_.register_tool(agent::make_read_resource_tool(mcp_servers_));
    mcp_servers_.connect_all();
    for (const auto& st : mcp_servers_.snapshot())
        if (st.connected) agent::register_server_tools(reg_, mcp_servers_,
                                                       st.name);

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
    router_->request_cancel();
    {
        std::scoped_lock lk(router_->mutex());
        router_->set_shutting_down(true);
        deny_all_pending_approvals(router_->queue());
        deny_all_pending_approvals(router_->pending_approvals());
        deny_all_pending_api_keys(router_->pending_api_keys());
    }
    if (router_->thread().joinable()) router_->thread().join();
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

Window& Tui::win() { return window_manager_->win(); }
const Window& Tui::win() const { return window_manager_->win(); }

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
        while (end < raw.size() && raw[end] != ' ' && raw[end] != '\t' &&
               raw[end] != ',' && raw[end] != '.' && raw[end] != '!' &&
               raw[end] != '?' && raw[end] != ';' && raw[end] != ':')
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
                    if (content.size() > 4096) content.resize(4096);
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
    if (router_->busy()) return;

    if (router_->thread().joinable())
        router_->thread().join();
    router_->clear();

    router_->set_busy(true);
    render_engine_->mark_working();
    router_->clear_cancel();

    ensure_chat_window();
    append_line(P_USER, "> " + raw_prompt);

    auto& w = win();
    w.reason_buf.clear();
    w.reason_folded = false;
    render_engine_->set_show_reasoning(cfg_.show_reasoning);
    w.stream_ts = timestamp();

    std::string prompt = expand_at_references(raw_prompt);

    // Capture the window on the UI thread: the worker must never read
    // window_manager_->all()/window_manager_->active() (the UI thread mutates them) — it gets its own Window*
    // and stamps every event with the window's stable id so drain_events can
    // route even after other windows close.
    Window* my_win = &w;
    size_t my_id = w.id;
    router_->thread() = std::thread([this, my_win, my_id, prompt] {
        agent_worker(*my_win, my_id, prompt);
    });
}



















void Tui::agent_worker(Window& my_win, size_t window_id,
                       const std::string& prompt) {
    agent::AgentHooks hooks = router_->make_hooks(window_id);

    // ONE subscription per window keeps the estimate fresh for the gauge;
    // ctx_used_ holds the server-reported truth and is never clobbered by
    // the chars/4 estimate (see gauge_tokens: server wins when known).
    my_win.agent->context_events().subscribe(
        [this](size_t tokens, size_t) {
            ctx_estimate_ = static_cast<long>(tokens);
        });

    try {
        if (!router_->cancel_requested()) {
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
    router_->set_busy(false);
}

void Tui::compress_worker(Window& my_win, size_t window_id) {
    if (router_->thread().joinable())
        router_->thread().join();
    router_->set_busy(true);
    compressing_ = true;
    render_engine_->mark_working();
    router_->thread() = std::thread([this, my_win = &my_win, window_id]() {
        AgentEvent ev = run_compression(*my_win, window_id);
        router_->push(std::move(ev));
        router_->set_busy(false);
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
    render_engine_->git_refresh();
    render_engine_->draw();
    render_engine_->draw_input("");
    render_engine_->flush();
    detect_server(false);
    timeout(kTickTimeoutMs);

    // Build the setting registry and command tree FIRST, then merge the
    // live feeds. Merging feeds before the rebuild wiped their leaves
    // (models, policy rules, providers, job ids) from the tree.
    build_settings();
    (void)commands();  // force command tree build
    // Load completion metadata from JSON (help text, choices, ranges).
    // This is the single source of truth for completion metadata — code edits
    // cannot break completion unless the JSON file is damaged.
    refresh_completions();
    refresh_model_list();
    refresh_policy_feed();
    refresh_job_feed();
    refresh_provider_feed();
    refresh_plugin_feed();

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
                prefix = input;                       // explicit descend
            } else {
                size_t tok_start = input.rfind(' ');
                tok_start = (tok_start == std::string::npos) ? 1
                                                            : tok_start + 1;
                std::string last_tok = input.substr(tok_start);
                // drawer_entry_names descends into a namespace when the
                // trailing token resolves to children (drawer_rows.cpp); in
                // that case the namespace is KEPT (prefix = input + " ").
                // Otherwise the token is a partial being typed and is
                // REPLACED (prefix = input minus the token).
                std::string ns_so_far = input.substr(1, tok_start - 1);
                while (!ns_so_far.empty() && ns_so_far.back() == ' ')
                    ns_so_far.pop_back();
                std::string probe = ns_so_far.empty()
                                        ? last_tok
                                        : ns_so_far + "." + last_tok;
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
        for (const auto& a : settings_.top_level_aliases()) names.push_back(a);
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
            router_->request_cancel();
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
            if (router_->thread().joinable() && !router_->busy()) {
                router_->thread().join();
                session_controller_->save_window_sessions();
            }
            session_controller_->save_workspace_now();
            _Exit(128 + g_signal_state.signal());
        }
        bool had_events = drain_events();
        jobs_.check_timeouts();
        // Time-driven plugin work (a provider's balance refresh, say). The bar
        // reads what the tick cached; segments themselves never fetch.
        plugin_runtime_.tick();
        if (!input_fill_.empty()) {
            cl.set_text(input_fill_);
            input_fill_.clear();
        }

        int ch = getch();
        if (ch == ERR) {
            if (had_events) { draw(); }
            else {
                auto now = std::chrono::steady_clock::now();
                if (now - render_engine_->last_status_tick() > std::chrono::milliseconds(150)) {
                    render_engine_->set_last_status_tick(now);
                    if (router_->busy()) {
                        router_->advance_tool_spinners();
                        render_engine_->draw();
                    } else {
                        router_->advance_tool_spinners();
                        render_engine_->tick_clock();
                    }
                }
            }
            render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
            if (!router_->busy() && !pending_prompt_.empty()) {
                std::string p = std::move(pending_prompt_);
                send_async(p);
            }
            if (render_engine_->dirty()) render_engine_->flush();
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

        // Alt+1..9 window switch (meta-encoded).
        if (ch >= 0xB1 && ch <= 0xB9) {
            switch_to(static_cast<size_t>(ch - 0xB1));
            draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }
        if (ch == 14 && !router_->busy()) {
            new_window("chat");
            render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow()); continue;
        }

        // ESC handling — toggle scroll mode or window switch.
        if (ch == 27) {
            if (cl.drawer_open()) {
                render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            // Alt+digit arrives as ESC then the digit (xterm default). Give
            // the follow-up read a brief window beyond the 50ms tick so the
            // digit reliably arrives; set_escdelay(25) lets a lone ESC fall
            // through quickly.
            timeout(200);
            int n = getch();
            timeout(kTickTimeoutMs);
            if (n >= '1' && n <= '9') {
                switch_to(static_cast<size_t>(n - '1'));
                render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            if (n == '0') {
                open_panels("");
                render_engine_->draw();
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            if (n == 'b' || n == 'B') {
                cl.on_ctrl_w();
                render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            // Cancel when agent is busy (ESC alone).
            if (router_->busy()) {
                cfg_.cancel_token.request();
                router_->request_cancel();
                append_line(P_STATUS, "cancelling…");
                render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            // Plain ESC (not busy, no key within timeout): toggle scroll mode.
            render_engine_->set_scroll_mode(!render_engine_->scroll_mode());
            if (render_engine_->scroll_mode())
                append_line(P_STATUS, "scroll mode — arrows/PgUp/PgDn navigate window");
            render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }

        // Ctrl+C: cancel or save+exit.
        if (ch == 3) {
            if (router_->busy()) {
                cfg_.cancel_token.request();
                router_->request_cancel();
                append_line(P_STATUS, "cancelling…");
                render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
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
                        win().scroll_top, delta,
                        render_engine_->max_scroll(win()));
                    render_engine_->draw();
                    render_engine_->draw_input(cl.text(), cl.cursor(),
                                               cl.shadow());
                }
            }
            continue;
        }

        // ── Route through CommandLine (pure logic, unit tested) ─
        CommandLine::Result result;
        bool handled = true;

        switch (ch) {
        case '	':            result = cl.on_tab(); break;
        case KEY_UP:
            if (render_engine_->scroll_mode())
                { win().scroll_top = std::max(0, win().scroll_top - 1); render_engine_->draw(); }
            else
                result = cl.on_up();
            break;
        case KEY_DOWN:
            if (render_engine_->scroll_mode())
                { win().scroll_top += 1; render_engine_->draw(); }
            else
                result = cl.on_down();
            break;
        case KEY_LEFT:          result = cl.on_left(); break;
        case KEY_RIGHT:         result = cl.on_right(); break;
        case KEY_HOME:          result = cl.on_home(); break;
        case KEY_END:           result = cl.on_end(); break;
        case KEY_PPAGE:
            if (render_engine_->scroll_mode())
                { win().scroll_top = std::max(0, win().scroll_top - 10); render_engine_->draw(); }
            break;
        case KEY_NPAGE:
            if (render_engine_->scroll_mode())
                { win().scroll_top = std::min(render_engine_->max_scroll(), win().scroll_top + 10); render_engine_->draw(); }
            break;
        case KEY_BACKSPACE: case 127: case 8: result = cl.on_backspace(); break;
        case 10: case 13: case KEY_ENTER:
            if (render_engine_->scroll_mode()) { render_engine_->set_scroll_mode(false); render_engine_->draw(); }
            result = cl.on_enter();
            break;
        case 1:  result = cl.on_ctrl_a(); break;
        case 5:  result = cl.on_ctrl_e(); break;
        case 11: result = cl.on_ctrl_k(); break;
        case 20: result = cl.on_ctrl_t(); break;
        case 21: result = cl.on_ctrl_u(); break;
        case 23: result = cl.on_ctrl_w(); break;
        case 25: result = cl.on_ctrl_y(); break;
        case 31: result = cl.on_undo();   break;
        case 4:  result = cl.on_ctrl_d(); break;
        case 18:
            append_line(P_STATUS, "Ctrl-R: not yet implemented");
            render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
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
                    if (ph.size() > 100) ph.erase(ph.begin());
                }
                win().history_pos = ph.size();
                cl.set_history(ph);
                if (handle_slash(text)) {
                    render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                    continue;
                }
                if (router_->busy()) {
                    pending_prompt_ = text;
                    append_line(P_STATUS, "queued");
                } else {
                    ensure_chat_window();
                    send_async(text);
                }
                render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            case CommandLine::Result::ShowPopup: {
                if (!cl.text().empty() && cl.text().back() == '@') {
                    namespace fs = std::filesystem;
                    std::string root = agent::Workspace::root();
                    std::vector<std::string> items;
                    for (const auto& e : fs::directory_iterator(root)) {
                        std::string name = e.path().filename().string();
                        if (name.front() == '.') continue;
                        if (fs::is_directory(e)) name += "/";
                        items.push_back(name);
                    }
                    std::sort(items.begin(), items.end());
                    if (!items.empty()) {
                        int sel = menu_select("reference file:", items);
                        if (sel >= 0 && sel < static_cast<int>(items.size())) {
                            std::string ref = items[sel];
                            if (ref.back() == '/') ref.pop_back();
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
                render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
    case CommandLine::Result::ShowHelpPage: {
        std::string node = result.help_node;
        if (!node.empty() && node[0] == '/') node = node.substr(1);
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
                    if (i > 0) line += ", ";
                    line += ch_choices[i];
                }
                page.push_back(line);
            }
            double rlo, rhi;
            if (settings_.range_for(help_key, rlo, rhi))
                page.push_back("range: " + std::to_string((int)rlo) +
                           " – " + std::to_string((int)rhi));
            info_dialog(help_key, page);
            redraw_after_modal();
            render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
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
                for (const auto& c : chc) msg += c + "|";
                msg.pop_back();
            }
            double rlo, rhi;
            if (settings_.range_for(help_key, rlo, rhi))
                msg += "  range: " + std::to_string(rlo) + "-" + std::to_string(rhi);
            append_line(P_STATUS, msg);
            render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }
        // Fallback to cmd_help for top-level commands.
        size_t sp = node.find(' ');
        if (sp != std::string::npos) node.resize(sp);
        slash_dispatcher_->cmd_help(node);
        render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
        continue;
    }
            default:
                break;
            }
            render_engine_->draw();
            render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());
            if (render_engine_->dirty()) { render_engine_->flush(); render_engine_->clear_dirty(); }
            continue;
        }

        // Unhandled keys.
        if (ch == KEY_NPAGE) { win().scroll_top += render_engine_->lines_per_page(); render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow()); continue; }
        if (ch == KEY_PPAGE) { win().scroll_top = std::max(0, win().scroll_top - render_engine_->lines_per_page()); render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow()); continue; }
        if (render_engine_->dirty()) { render_engine_->flush(); render_engine_->clear_dirty(); }
    }
}



void Tui::redraw_after_modal() { session_controller_->redraw_after_modal(); }
void Tui::autosave() { session_controller_->autosave(); }
void Tui::autosave(Window& w) { session_controller_->autosave(w); }
void Tui::load_session(const std::string& id) { session_controller_->load_session(id); }
void Tui::draw() { render_engine_->draw(); }
void Tui::draw_input(const std::string& s, size_t cursor, const std::string& shadow) { render_engine_->draw_input(s, cursor, shadow); }
void Tui::build_settings() { slash_dispatcher_->build_settings(); }
bool Tui::drain_events() { return router_->drain_events(); }
const std::vector<tui::Command>& Tui::commands() { return slash_dispatcher_->commands(); }
bool Tui::handle_slash(const std::string& line) { return slash_dispatcher_->handle_slash(line); }
void Tui::register_action(const std::string& action,
                          std::function<void(const std::string&)> handler) {
    slash_dispatcher_->register_action(action, std::move(handler));
}
bool Tui::busy_reject(const std::string& what) {
    return slash_dispatcher_->busy_reject(what);
}
void Tui::refresh_completions() { slash_dispatcher_->refresh_completions(); }
void Tui::refresh_model_list() { slash_dispatcher_->refresh_model_list(); }
void Tui::refresh_policy_feed() { slash_dispatcher_->refresh_policy_feed(); }
void Tui::refresh_provider_feed() { slash_dispatcher_->refresh_provider_feed(); }
void Tui::refresh_job_feed() { slash_dispatcher_->refresh_job_feed(); }
void Tui::refresh_plugin_feed() {
    if (feed_manager_) feed_manager_->refresh_plugin_feed();
}
void Tui::cmd_model_set(const std::string& arg) { slash_dispatcher_->cmd_model_set(arg); }
void Tui::cmd_provider(const std::string& arg) { slash_dispatcher_->cmd_provider(arg); }
void Tui::show_plugin(const std::string& id) { slash_dispatcher_->show_plugin(id); }
void Tui::open_panels(const std::string& id) {
    const std::string shown = panel_view(plugin_runtime_.panels(), id);
    if (shown.empty()) {
        append_line(P_STATUS, "no panels registered");
        return;
    }
    // The panel view owns the screen while it is up; repaint around it.
    redraw_after_modal();
}
void Tui::job_kill(const std::string& id) { slash_dispatcher_->job_kill(id); }
void Tui::job_read(const std::string& id) { slash_dispatcher_->job_read(id); }
void Tui::apply_policy_rule(const std::string& name, const std::string& lvl) { slash_dispatcher_->apply_policy_rule(name, lvl); }
void Tui::show_policy_rule(const std::string& name) { slash_dispatcher_->show_policy_rule(name); }

Window* Tui::window_by_id(size_t id) {
    return find_window(window_manager_->all(), id);
}

} // namespace tui
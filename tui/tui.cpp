// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "command_line.h"
#include "confirm_panel.h"
#include "drawer_rows.h"
#include "tool_display.h"
#include "signal_guard.h"
#include "event_router.h"

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
// One-line compression result summary for the status bar.
std::string compress_summary(const agent::CompressionResult& r) {
    if (r.messages_before == 0)
        return "compress: no compressor configured";
    if (r.messages_after >= r.messages_before)
        return "compress: nothing to prune ("
               + std::to_string(r.messages_before)
               + " messages, ~" + std::to_string(r.tokens_before)
               + " tokens)";
    return "compress: " + std::to_string(r.messages_before)
           + " \u2192 " + std::to_string(r.messages_after)
           + " msgs, ~" + std::to_string(r.tokens_before)
           + " \u2192 ~" + std::to_string(r.tokens_after) + " tokens"
           + "  (core:" + std::to_string(r.core_count)
           + " ctx:" + std::to_string(r.context_count)
           + " prune:" + std::to_string(r.prune_count) + ")";
}

} // namespace

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
          agent::PluginRegistry& plugin_reg)
    : cfg_(std::move(cfg)),
      providers_(agent::make_default_provider_service(cfg_)),
      reg_(reg), jobs_(jobs), subagents_(subagents),
      plugins_(plugins), plugin_reg_(plugin_reg),
      mcp_servers_(agent::load_mcp_servers(), &this->cfg_.cancel_token),
      settings_path_(agent::Workspace::local_dir() + "/settings") {
    std::setlocale(LC_ALL, "");
    g_terminal_guard.capture();
    initscr();
    raw();        // capture Ctrl-C as keypress (ASCII 3) instead of SIGINT
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(1);
    start_color();
    // Enable xterm alternate scroll mode so the mouse wheel sends cursor keys
    // instead of mouse events.  This keeps native text selection working
    // (click-and-drag, Cmd-C) while still letting the wheel scroll the chat.
    // Without this, mousemask() would intercept all mouse input and break
    // terminal-level selection on macOS and Linux.
    std::fputs("\033[?1007h", stdout);
    std::fflush(stdout);
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

    reg_.register_tool(agent::make_read_resource_tool(mcp_servers_));
    mcp_servers_.connect_all();
    for (const auto& st : mcp_servers_.snapshot())
        if (st.connected) agent::register_server_tools(reg_, mcp_servers_,
                                                       st.name);

    // Restore previous workspace: open saved sessions in their own windows.
    // On first launch (no saved workspace) show the welcome mural instead.
    auto ws = store_.load_workspace();
    if (!ws.windows.empty()) {
        for (const auto& we : ws.windows) {
            Window& w = new_window(we.title.empty() ? "chat" : we.title);
            w.session_id = we.session_id;
            w.prompt_history = we.prompt_history;
            w.history_pos = w.prompt_history.size();
        }
        if (ws.active < windows_.size())
            active_ = ws.active;
        lazy_load_active();
    } else {
        open_welcome_window();
    }
}

Tui::~Tui() {
    std::fputs("\033[?1007l", stdout);
    std::fflush(stdout);

    agent_cancel_ = true;
    // Resolve every pending approval (queued and undrained) with Deny so a
    // worker blocked on its promise can finish and be joined; otherwise the
    // join below deadlocks forever.
    {
        std::scoped_lock lk(event_mtx_);
        shutting_down_ = true;
        deny_all_pending_approvals(event_queue_);
        deny_all_pending_approvals(pending_approvals_);
    }
    // Join FIRST, then snapshot: snapshot(w) reads the agent's context, which
    // the worker mutates — saving before the join races the worker and can
    // persist a torn conversation. endwin() before the stderr progress so the
    // save messages do not write while ncurses owns the screen. Sessions
    // before the workspace: the workspace records session_ids that
    // save_window_sessions may just have (re)assigned.
    if (agent_thread_.joinable()) agent_thread_.join();
    endwin();
    save_window_sessions();
    save_workspace_now();
}

Window& Tui::new_window(const std::string& title) {
    auto w = std::make_unique<Window>();
    w->id = next_window_id_++;
    w->title = title;
    auto comp_cfg = agent::load_compression_config(cfg_);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto exp_cfg = agent::load_experience_config(cfg_);
    auto mem_store = agent::make_memory_store(exp_cfg);
    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem_store);
    agent::Agent a(cfg_, reg_,
        agent::AgentHooks{},
        std::move(compressor), std::move(gate),
        std::move(mem_store), std::move(retriever),
        {}, {}, true);
    w->agent = std::make_unique<agent::Agent>(std::move(a));
    w->agent->policy().init(agent::Workspace::local_dir() + "/policy.json");
    windows_.push_back(std::move(w));
    active_ = windows_.size() - 1;
    return *windows_.back();
}

Window& Tui::open_welcome_window() {
    auto w = std::make_unique<Window>();
    w->id = next_window_id_++;
    w->title = "amber";
    w->read_only = true;
    w->welcome_art = true;
    windows_.push_back(std::move(w));
    active_ = windows_.size() - 1;
    return *windows_.back();
}

Window& Tui::ensure_chat_window() {
    if (!win().read_only) return win();
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (!windows_[i]->read_only) { switch_to(i); return win(); }
    }
    return new_window("chat");
}

Window& Tui::win() { return *windows_[active_]; }
const Window& Tui::win() const { return *windows_[active_]; }

// ---- thread / event machinery -------------------------------------------

bool Tui::drain_events() {
    std::vector<AgentEvent> batch;
    {
        std::scoped_lock lk(event_mtx_);
        while (!event_queue_.empty()) {
            batch.push_back(std::move(event_queue_.front()));
            event_queue_.pop();
        }
    }

    if (batch.empty()) return false;

    for (auto& ev : batch) {
        // Deliver to the event's origin window (npos = active at drain time).
        // Window-state cases need w and skip when it is gone; global-state
        // cases (StateChange, Stats) and active-window Status lines still run.
        Window* w = route_event(windows_, ev, active_);
        switch (ev.type) {
        case AgentEvent::StateChange:
            state_ = ev.state;
            if (ev.state == agent::RunState::Idle ||
                ev.state == agent::RunState::Error)
                running_tool_.clear();
            break;
        case AgentEvent::Reasoning:
            on_reasoning(w, ev);
            break;
        case AgentEvent::Token:
            on_token(w, ev);
            break;
        case AgentEvent::Status:
            // Worker status belongs to the conversation that emitted it.
            if (w) append_line_to(*w, P_STATUS, ev.text);
            break;
        case AgentEvent::ToolCall:
            on_tool_call(w, ev);
            break;
        case AgentEvent::ToolResult:
            on_tool_result(w, ev);
            break;
        case AgentEvent::Assistant:
            on_assistant(w, ev);
            break;
        case AgentEvent::Stats:
            stats_ = ev.stats;
            if (ev.stats.prompt_tokens >= 0) {
                ctx_used_ = ev.stats.prompt_tokens;
                live_ctx_offset_ = 0;
            }
            break;
        case AgentEvent::Error:
            on_error(w, ev);
            break;
        case AgentEvent::Done:
            on_done(w, ev);
            break;
        case AgentEvent::CompressResult:
            on_compress_result(w, ev);
            break;
        case AgentEvent::Approval: {
            // While a modal dialog is open the main thread is not in the event
            // loop; queue the approval so we don't nest ncurses dialogs or
            // deadlock the worker on its promise. Resolved in
            // redraw_after_modal() once the modal closes.
            if (modal_open_) {
                pending_approvals_.push(std::move(ev));
                break;
            }
            resolve_approval(ev);
            break;
        }
        }
    }

    pump_pending_approvals();
    return true;
}

void Tui::pump_pending_approvals() {
    // Resolve any approvals queued while a modal was open. Only one per pump
    // so a nested approval (during the approval dialog) queues and resolves
    // on a later tick rather than re-entering this loop.
    if (modal_open_ || pending_approvals_.empty()) return;
    AgentEvent ev = std::move(pending_approvals_.front());
    pending_approvals_.pop();
    resolve_approval(ev);
}

void Tui::resolve_approval(const AgentEvent& ev) {
    agent::Approval d = approve_dialog(ev.text, policy_timeout_, 0);
    const char* verdict = "denied";
    if (d == agent::Approval::AllowOnce) verdict = "allowed once";
    else if (d == agent::Approval::AllowSession) verdict = "allowed session";
    else if (d == agent::Approval::AlwaysAllow) verdict = "always allow";
    else if (d == agent::Approval::AlwaysDeny) verdict = "always deny";
    append_line(P_STATUS,
                std::string("approval: ") + verdict + "  (" + ev.text + ")");
    if (ev.approval_promise)
        ev.approval_promise->set_value(d);
}


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
            std::string root = agent::Workspace::root();
            fs::path ref_path = fs::path(root) / ref;
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
        i = end;
    }
    return out;
}

void Tui::send_async(const std::string& raw_prompt) {
    if (agent_busy_.load()) return;

    if (agent_thread_.joinable())
        agent_thread_.join();
    // The previous worker is joined; drop any of its events still queued
    // (including its Done) so a stale Done can't reset the new turn's state.
    {
        std::scoped_lock lk(event_mtx_);
        std::queue<AgentEvent> empty;
        std::swap(event_queue_, empty);
    }

    agent_busy_.store(true);
    working_since_ = std::chrono::steady_clock::now();
    working_visible_ = true;
    agent_cancel_.store(false);

    ensure_chat_window();
    append_line(P_USER, "> " + raw_prompt);

    auto& w = win();
    w.reason_buf.clear();
    w.reason_folded = false;
    show_reasoning_ = cfg_.show_reasoning;
    w.stream_ts = timestamp();

    std::string prompt = expand_at_references(raw_prompt);

    // Capture the window on the UI thread: the worker must never read
    // windows_/active_ (the UI thread mutates them) — it gets its own Window*
    // and stamps every event with the window's stable id so drain_events can
    // route even after other windows close.
    Window* my_win = &w;
    size_t my_id = w.id;
    agent_thread_ = std::thread([this, my_win, my_id, prompt] {
        agent_worker(*my_win, my_id, prompt);
    });
}

void Tui::on_reasoning(Window* w, const AgentEvent& ev) {
    if (!w) return;
    w->reason_buf += ev.text;
    if (!w->reason_folded && show_reasoning_)
        w->scroll_top = max_scroll(*w);
}

void Tui::on_token(Window* w, const AgentEvent& ev) {
    if (!w) return;
    working_visible_ = false;  // output is displaying — row retires
    if (!w->reason_folded && !w->reason_buf.empty())
        fold_reasoning(*w);
    w->stream_color = P_ASSISTANT;
    w->stream_buf += ev.text;
    live_ctx_offset_ += (static_cast<long>(ev.text.size()) / 4) + 1;
    w->scroll_top = max_scroll(*w);
}

void Tui::on_tool_call(Window* w, const AgentEvent& ev) {
    if (!w) return;
    running_tool_ = ev.tool_name;
    running_tool_desc_ = tool_display::describe_tool_call(
        ev.tool_name, ev.tool_args);
    working_visible_ = true;
    flush_stream(*w);
    // One "open" line per advertised call, animated together: round
    // spinner + a human description of what is actually running
    // (bash -> the command, read/write -> path, search -> pattern).
    PendingToolLine pt;
    pt.name = ev.tool_name;
    pt.fingerprint = ev.tool_args.dump();
    pt.frame = 0;
    pt.tail = " " + tool_display::describe_tool_call(ev.tool_name,
                                                     ev.tool_args);
    const char* frame = text::glyph::spinner_round(0);
    pt.window_id = ev.window_id;
    pt.index = append_line_to(*w, P_STATUS,
                              std::string(frame) + pt.tail);
    pending_tools_.push_back(std::move(pt));
}

void Tui::on_tool_result(Window* w, const AgentEvent& ev) {
    if (!w) return;
    running_tool_.clear();
    running_tool_desc_.clear();
    // Summary line: colored success/failure icon + one-line report,
    // closed IN PLACE on the open line (single line per tool call).
    rich::Line summary = tool_display::result_line(
        ev.tool_name, ev.tool_args, ev.tool_result.ok,
        ev.tool_result.output, ev.tool_result.error);
    // Match the pending line for this call (same window + name + args, FIFO).
    size_t match = find_pending_tool(ev.window_id, ev.tool_name,
                                     ev.tool_args.dump());
    if (match != std::string::npos) {
        auto& pt = pending_tools_[match];
        Window* ow = window_by_id(pt.window_id);
        if (ow && pt.index < ow->lines.size()) {
            size_t li = pt.index;
            // Close in place: keep the open line's timestamp run.
            ow->lines[li] = tool_display::close_tool_line(
                ow->lines[li], std::move(summary));
            pending_tools_.erase(pending_tools_.begin() + match);
        } else {
            append_rich_to(*w, summary);
        }
    } else {
        append_rich_to(*w, summary);
    }
    // Tool may have modified files — refresh git state for prompt.
    git_refresh();
}

void Tui::on_assistant(Window* w, const AgentEvent& ev) {
    if (!w) return;
    if (w->stream_buf.empty())
        append_markdown(*w, ev.text);
}

void Tui::on_error(Window* w, const AgentEvent& ev) {
    if (!w) return;
    state_ = agent::RunState::Error;
    flush_stream(*w);
    append_line_to(*w, P_STATUS, std::string("error: ") + ev.error_msg);
}

void Tui::on_done(Window* w, const AgentEvent& ev) {
    if (!w) return;
    if (state_ != agent::RunState::Error)
        state_ = agent::RunState::Idle;
    running_tool_.clear();
    flush_stream(*w);
    w->dirty = true;
    autosave(*w);
}

void Tui::on_compress_result(Window* w, const AgentEvent& ev) {
    if (!w) return;
    auto& r = ev.compress_result;
    state_ = agent::RunState::Idle;
    ctx_used_ = static_cast<long>(r.tokens_after);
    append_line_to(*w, P_STATUS, compress_summary(r));
    w->dirty = true;
}

agent::AgentHooks Tui::make_agent_hooks(size_t window_id) {
    agent::AgentHooks hooks;

    // Push one event for the origin window unless cancellation is pending.
    auto push_event = [this, window_id](AgentEvent ev) {
        if (agent_cancel_.load()) return;
        ev.window_id = window_id;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };

    hooks.on_reasoning = [push_event](const std::string& d) {
        AgentEvent ev;
        ev.type = AgentEvent::Reasoning;
        ev.text = d;
        push_event(std::move(ev));
    };
    hooks.on_token = [push_event](const std::string& d) {
        AgentEvent ev;
        ev.type = AgentEvent::Token;
        ev.text = d;
        push_event(std::move(ev));
    };
    hooks.on_state = [push_event](agent::RunState s) {
        AgentEvent ev;
        ev.type = AgentEvent::StateChange;
        ev.state = s;
        push_event(std::move(ev));
    };
    hooks.on_stats = [push_event](const agent::Stats& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Stats;
        ev.stats = s;
        push_event(std::move(ev));
    };
    hooks.on_status = [push_event](const std::string& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Status;
        ev.text = s;
        push_event(std::move(ev));
    };
    hooks.on_tool_call = [push_event](const std::string& n, const agent::json& a) {
        AgentEvent ev;
        ev.type = AgentEvent::ToolCall;
        ev.tool_name = n;
        ev.tool_args = a;
        push_event(std::move(ev));
    };
    hooks.on_tool_result = [push_event](const std::string& n,
                                        const agent::ToolResult& r,
                                        const agent::json& a) {
        AgentEvent ev;
        ev.type = AgentEvent::ToolResult;
        ev.tool_name = n;
        ev.tool_args = a;
        ev.tool_result = r;
        push_event(std::move(ev));
    };
    hooks.on_assistant = [push_event](const std::string& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Assistant;
        ev.text = s;
        push_event(std::move(ev));
    };
    hooks.on_approval = [this, window_id](const std::string& name,
                                          const agent::json& args,
                                          const std::string& summary) -> agent::Approval {
        if (agent_cancel_.load()) return agent::Approval::Deny;
        auto p = std::make_shared<std::promise<agent::Approval>>();
        auto f = p->get_future();
        AgentEvent ev;
        ev.type = AgentEvent::Approval;
        ev.text = summary;
        ev.approval_promise = p;
        {
            // The teardown check and the enqueue must be atomic: a worker
            // that checked agent_cancel_ before the destructor drained the
            // queues would push an approval nothing ever resolves and block
            // this thread on f.get() forever, hanging ~Tui's join().
            std::scoped_lock lk(event_mtx_);
            if (shutting_down_) return agent::Approval::Deny;
            ev.window_id = window_id;
            event_queue_.push(std::move(ev));
        }
        (void)name;
        (void)args;
        return f.get();
    };

    return hooks;
}

void Tui::agent_worker(Window& my_win, size_t window_id,
                       const std::string& prompt) {
    agent::AgentHooks hooks = make_agent_hooks(window_id);

    // Subscribe to context change events for live token count updates.
    my_win.agent->context_events().subscribe(
        [this](size_t tokens, size_t) {
            ctx_used_ = static_cast<long>(tokens);
        });

    try {
        if (!agent_cancel_.load()) {
            my_win.agent->set_hooks(hooks);
            my_win.agent->run(prompt);
            my_win.dirty = true;
        }
    } catch (const std::exception& e) {
        AgentEvent ev;
        ev.type = AgentEvent::Error;
        ev.window_id = window_id;
        ev.error_msg = e.what();
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    } catch (...) {
        // A non-std exception must never escape a thread entry (terminate).
        AgentEvent ev;
        ev.type = AgentEvent::Error;
        ev.window_id = window_id;
        ev.error_msg = "agent thread terminated unexpectedly";
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    }

    AgentEvent done;
    done.type = AgentEvent::Done;
    done.window_id = window_id;
    {
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(done));
    }
    agent_busy_.store(false);
}

void Tui::compress_worker(Window& my_win, size_t window_id) {
    // Compression runs on the same worker thread as the agent loop, so the
    // destructor's join() covers it too — never a detached thread capturing
    // `this` (use-after-free when the TUI is torn down mid-compression).
    if (agent_thread_.joinable())
        agent_thread_.join();
    agent_busy_.store(true);
    working_since_ = std::chrono::steady_clock::now();
    working_visible_ = true;
    agent_thread_ = std::thread([this, my_win = &my_win, window_id]() {
        AgentEvent ev = run_compression(*my_win, window_id);
        {
            std::scoped_lock lk(event_mtx_);
            event_queue_.push(std::move(ev));
        }
        // Push BEFORE clearing busy: send_async observes idle, joins, then
        // drains the queue — an event pushed after the flag flip is lost.
        agent_busy_.store(false);
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

void Tui::git_refresh() {
    auto read_stdout = [](const char* cmd) -> std::string {
        std::string result;
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return result;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe))
            result += buf;
        pclose(pipe);
        return result;
    };

    // Project name from cwd basename.
    {
        std::string cwd;
        std::array<char, 4096> buf;
        if (getcwd(buf.data(), buf.size()))
            cwd = buf.data();
        size_t slash = cwd.rfind('/');
        git_project_ = (slash == std::string::npos) ? cwd : cwd.substr(slash + 1);
        if (git_project_.empty()) git_project_ = "project";
    }

    std::string ref = read_stdout("git symbolic-ref HEAD 2>/dev/null");
    if (ref.empty()) {
        git_branch_.clear();
        git_ins_ = 0;
        git_del_ = 0;
        return;
    }
    ref.erase(ref.find_last_not_of(" \n\r") + 1);
    if (ref.compare(0, 11, "refs/heads/") == 0)
        git_branch_ = ref.substr(11);
    else
        git_branch_ = ref;

    std::string stat = read_stdout(
        "git diff --shortstat -- . ':!bench/results' ':!*.o' ':!*.a' ':!*.d' 2>/dev/null");
    git_ins_ = 0;
    git_del_ = 0;
    if (!stat.empty()) {
        auto extract = [&](const std::string& needle) -> int {
            size_t pos = stat.find(needle);
            if (pos == std::string::npos) return 0;
            size_t start = pos;
            while (start > 0 && isdigit(static_cast<unsigned char>(stat[start - 1])))
                --start;
            return std::stoi(stat.substr(start, pos - start));
        };
        git_ins_ = extract(" insertion");
        git_del_ = extract(" deletion");
    }
}

void Tui::refresh_completions() {
    settings_.reset_completion_index();
    auto try_load = [&](const std::string& path) {
        if (path.empty()) return false;
        bool ok = settings_.load_completions_json(path);
        if (ok) append_line(P_DEBUG, "loaded completions from " + path);
        return ok;
    };
    // Search order: CWD, binary dir, workspace, user data dirs, system data
    // dirs — shared with prompt resolution so packaged installs work from any
    // working directory.
    std::string exed = agent::exe_dir();
    for (const auto& c : agent::data_file_candidates(
             "completions.json", exed.empty() ? nullptr : exed.c_str()))
        if (try_load(c)) break;
    // Plugin namespaces merge on top so their commands complete and show help
    // like core ones.
    for (const auto& p : plugins_.plugins())
        if (p.state == agent::PluginState::Enabled)
            settings_.merge_completions_json(p.manifest.completion);
    // Live MCP tools get their own <server> branches under the mcp command.
    settings_.merge_completions_json(agent::mcp_completion_subtree(reg_));
}

void Tui::run() {
    git_refresh();
    draw();
    draw_input("");
    flush();
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
            cl.set_completions(drawer_entry_names(input, settings_));
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
            agent_cancel_ = true;
            {
                std::scoped_lock lk(event_mtx_);
                shutting_down_ = true;
                deny_all_pending_approvals(event_queue_);
                deny_all_pending_approvals(pending_approvals_);
            }
            // endwin() first: the session-save progress writes to stderr and
            // must not interleave with a terminal ncurses still controls.
            endwin();
            if (agent_thread_.joinable() && !agent_busy_.load()) {
                agent_thread_.join();
                save_window_sessions();
            }
            save_workspace_now();
            _Exit(128 + g_signal_state.signal());
        }
        bool had_events = drain_events();
        jobs_.check_timeouts();
        if (!input_fill_.empty()) {
            cl.set_text(input_fill_);
            input_fill_.clear();
        }

        int ch = getch();
        if (ch == ERR) {
            if (had_events) { draw(); }
            else {
                auto now = std::chrono::steady_clock::now();
                if (now - last_status_tick_ > std::chrono::milliseconds(150)) {
                    last_status_tick_ = now;
                    if (agent_busy_.load()) {
                        // Full redraw while busy: the sticky working row and
                        // tool spinners animate even during a quiet wait when
                        // no events arrive (events alone used to leave the
                        // chat area stale until the first output token).
                        advance_tool_spinners();
                        draw();
                    } else {
                        advance_tool_spinners();
                        tick_clock();
                    }
                }
            }
            draw_input(cl.text(), cl.cursor(), cl.shadow());
            if (!agent_busy_.load() && !pending_prompt_.empty()) {
                std::string p = std::move(pending_prompt_);
                send_async(p);
            }
            if (dirty_) flush();
            continue;
        }

        // Alt+1..9 window switch (meta-encoded).
        if (ch >= 0xB1 && ch <= 0xB9) {
            switch_to(static_cast<size_t>(ch - 0xB1));
            draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }
        if (ch == 14 && !agent_busy_.load()) {
            new_window("chat");
            draw(); draw_input(cl.text(), cl.cursor(), cl.shadow()); continue;
        }

        // ESC handling — toggle scroll mode or window switch.
        if (ch == 27) {
            if (cl.drawer_open()) {
                draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            int n = getch();
            if (n >= '1' && n <= '9') {
                switch_to(static_cast<size_t>(n - '1'));
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            if (n == 'b' || n == 'B') {
                cl.on_ctrl_w();
                draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            // Cancel when agent is busy (ESC alone).
            if (agent_busy_.load()) {
                cfg_.cancel_token.request();
                agent_cancel_.store(true);
                append_line(P_STATUS, "cancelling…");
                draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            // Plain ESC (not busy, no key within timeout): toggle scroll mode.
            scroll_mode_ = !scroll_mode_;
            if (scroll_mode_)
                append_line(P_STATUS, "scroll mode — arrows/PgUp/PgDn navigate window");
            draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }

        // Ctrl+C: cancel or save+exit.
        if (ch == 3) {
            if (agent_busy_.load()) {
                cfg_.cancel_token.request();
                agent_cancel_.store(true);
                append_line(P_STATUS, "cancelling…");
                draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
                continue;
            }
            save_workspace_now();
            quit_ = true;
            break;
        }

        // ── Route through CommandLine (pure logic, unit tested) ─
        CommandLine::Result result;
        bool handled = true;

        switch (ch) {
        case '	':            result = cl.on_tab(); break;
        case KEY_UP:
            if (scroll_mode_)
                { win().scroll_top = std::max(0, win().scroll_top - 1); draw(); }
            else
                result = cl.on_up();
            break;
        case KEY_DOWN:
            if (scroll_mode_)
                { win().scroll_top += 1; draw(); }
            else
                result = cl.on_down();
            break;
        case KEY_LEFT:          result = cl.on_left(); break;
        case KEY_RIGHT:         result = cl.on_right(); break;
        case KEY_HOME:          result = cl.on_home(); break;
        case KEY_END:           result = cl.on_end(); break;
        case KEY_PPAGE:
            if (scroll_mode_)
                { win().scroll_top = std::max(0, win().scroll_top - 10); draw(); }
            break;
        case KEY_NPAGE:
            if (scroll_mode_)
                { win().scroll_top = std::min(max_scroll(), win().scroll_top + 10); draw(); }
            break;
        case KEY_BACKSPACE: case 127: case 8: result = cl.on_backspace(); break;
        case 10: case 13: case KEY_ENTER:
            if (scroll_mode_) { scroll_mode_ = false; draw(); }
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
            draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
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
            drawer_open_ = cl.drawer_open();
            drawer_sel_ = cl.drawer_sel();
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
                    draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
                    continue;
                }
                if (agent_busy_.load()) {
                    pending_prompt_ = text;
                    append_line(P_STATUS, "queued");
                } else {
                    ensure_chat_window();
                    send_async(text);
                }
                draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
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
                    auto matches = filter_commands(tok);
                    if (!matches.empty()) {
                        std::vector<std::string> items;
                        items.reserve(matches.size());
                        for (auto* c : matches)
                            items.emplace_back(palette::usage(*c) + "  " + c->help);
                        menu_select("options:", items);
                    }
                }
                draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
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
            draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
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
            draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
            continue;
        }
        // Fallback to cmd_help for top-level commands.
        size_t sp = node.find(' ');
        if (sp != std::string::npos) node.resize(sp);
        cmd_help(node);
        draw(); draw_input(cl.text(), cl.cursor(), cl.shadow());
        continue;
    }
            default:
                break;
            }
            // Redraw full screen to clear any previous drawer overlay content.
            draw();
            draw_input(cl.text(), cl.cursor(), cl.shadow());
            if (dirty_) { flush(); dirty_ = false; }
            continue;
        }

        // Unhandled keys.
        if (ch == KEY_NPAGE) { win().scroll_top += lines_per_page(); draw(); draw_input(cl.text(), cl.cursor(), cl.shadow()); continue; }
        if (ch == KEY_PPAGE) { win().scroll_top = std::max(0, win().scroll_top - lines_per_page()); draw(); draw_input(cl.text(), cl.cursor(), cl.shadow()); continue; }
        if (dirty_) { flush(); dirty_ = false; }
    }
}

size_t Tui::find_pending_tool(size_t window_id, const std::string& name,
                              const std::string& fingerprint) const {
    size_t fallback = std::string::npos;
    for (size_t i = 0; i < pending_tools_.size(); ++i) {
        if (pending_tools_[i].window_id != window_id ||
            pending_tools_[i].name != name)
            continue;
        if (pending_tools_[i].fingerprint == fingerprint) return i;
        if (fallback == std::string::npos) fallback = i;
    }
    return fallback;
}

void Tui::save_window_sessions() {
    for (const auto& window : windows_) {
        Window& w = *window;
        if (!w.dirty || !w.agent || w.agent->context().get_all().empty()) continue;
        std::fprintf(stderr, "\rsaving session '%s'...", w.title.c_str());
        std::fflush(stderr);
        agent::Session s = snapshot(w);
        if (store_.save(s)) w.session_id = s.id;
    }
    std::fprintf(stderr, "\rsession save complete\n");
}

Window* Tui::window_by_id(size_t id) {
    return find_window(windows_, id);
}

} // namespace tui

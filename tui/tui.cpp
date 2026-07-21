// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"

#include <agent.h>

#include "widgets.h"
#include "textutil.h"
#include "welcome.h"

#include <clocale>
#include <ctime>
#include <functional>

namespace tui {

Tui::Tui(agent::Config cfg, agent::ToolRegistry& reg, agent::JobService& jobs)
    : cfg_(std::move(cfg)), reg_(reg), jobs_(jobs) {
    std::setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(1);
    start_color();
    set_modal_flag(&modal_open_);
    use_default_colors();
    use_legacy_coding(1);
    init_pairs();
    open_welcome_window();
}

Tui::~Tui() {
    agent_cancel_ = true;
    if (agent_thread_.joinable()) agent_thread_.join();

    for (const auto & window : windows_) {
        Window& w = *window;
        if (!w.dirty || !w.agent || w.agent->history().empty()) continue;
        agent::Session s = snapshot(w);
        if (store_.save(s)) w.session_id = s.id;
    }
    endwin();
}

Window& Tui::new_window(const std::string& title) {
    auto w = std::make_unique<Window>();
    w->title = title;
    w->agent = std::make_unique<agent::Agent>(cfg_, reg_);
    windows_.push_back(std::move(w));
    active_ = windows_.size() - 1;
    return *windows_.back();
}

Window& Tui::open_welcome_window() {
    auto w = std::make_unique<Window>();
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
        switch (ev.type) {
        case AgentEvent::StateChange:
            state_ = ev.state;
            if (ev.state == agent::RunState::Idle ||
                ev.state == agent::RunState::Error)
                running_tool_.clear();
            break;
        case AgentEvent::Reasoning:
            win().reason_buf += ev.text;
            if (!win().reason_folded && show_reasoning_)
                win().scroll_top = max_scroll();
            break;
        case AgentEvent::Token:
            if (!win().reason_folded && !win().reason_buf.empty())
                fold_reasoning();
            win().stream_color = P_ASSISTANT;
            win().stream_buf += ev.text;
            win().scroll_top = max_scroll();
            break;
        case AgentEvent::Status:
            append_line(P_STATUS, ev.text);
            break;
        case AgentEvent::ToolCall: {
            running_tool_ = ev.tool_name;
            flush_stream();
            ToolFold fold = tool_fold_;
            if (fold != ToolFold::Never) {
                std::string args = ev.tool_args.dump();
                if (args.size() > 60) { args.resize(57); args += "..."; }
                if (fold == ToolFold::Auto)
                    append_line(P_STATUS, std::string(text::glyph::tool()) + " " +
                                ev.tool_name + " " + args);
                else
                    append_line(P_STATUS, "tool: " + ev.tool_name + " " + args);
            }
            break;
        }
        case AgentEvent::ToolResult: {
            running_tool_.clear();
            ToolFold fold = tool_fold_;
            if (fold == ToolFold::Never) break;
            // Build a compact summary line.
            auto summarize = [](const std::string& name,
                                const agent::ToolResult& r) -> std::string {
                const char* sp = text::glyph::tool();
                const char* ar = text::glyph::arrow();
                if (!r.ok) return std::string(sp) + " " + name + "  " +
                                   ar + " error: " + r.error;
                // Count lines in output.
                int lines = 1;
                for (char c : r.output) if (c == '\n') ++lines;
                std::string preview = r.output;
                size_t nl = preview.find('\n');
                if (nl != std::string::npos) preview.resize(nl);
                if (preview.size() > 60) { preview.resize(57); preview += "..."; }
                return std::string(sp).append(" ").append(name).append("  ")
                       .append(ar)
                       .append(" exit 0  (")
                       .append(std::to_string(lines))
                       .append(" lines)  ")
                       .append(preview);
            };
            std::string line = summarize(ev.tool_name, ev.tool_result);
            if (fold == ToolFold::Auto) {
                // Replace the last "running" tool line with the summary.
                auto& lines = win().lines;
                bool replaced = false;
                for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
                    if (!lines[i].runs.empty() &&
                        lines[i].runs[0].pair == P_STATUS &&
                        lines[i].runs[0].text.rfind(text::glyph::tool(), 0) == 0) {
                        lines[i].runs.clear();
                        rich::Run r; r.pair = P_STATUS; r.text = line;
                        lines[i].runs.push_back(r);
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) append_line(P_STATUS, line);
            } else {
                append_line(P_STATUS, line);
            }
            break;
        }
        case AgentEvent::Assistant:
            if (win().stream_buf.empty())
                append_markdown(ev.text);
            break;
        case AgentEvent::Stats:
            stats_ = ev.stats;
            if (ev.stats.prompt_tokens >= 0)
                ctx_used_ = ev.stats.prompt_tokens;
            break;
        case AgentEvent::Error:
            state_ = agent::RunState::Error;
            flush_stream();
            append_line(P_STATUS, std::string("error: ") + ev.error_msg);
            break;
        case AgentEvent::Done:
            if (state_ != agent::RunState::Error)
                state_ = agent::RunState::Idle;
            running_tool_.clear();
            flush_stream();
            autosave();
            win().dirty = true;
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

    // Resolve any approvals queued while a modal was open. Only one per pump so
    // a nested approval (during the approval dialog) queues and resolves on a
    // later tick rather than re-entering this loop.
    if (!modal_open_ && !pending_approvals_.empty()) {
        AgentEvent ev = std::move(pending_approvals_.front());
        pending_approvals_.pop();
        resolve_approval(ev);
    }
    return true;
}

void Tui::resolve_approval(const AgentEvent& ev) {
    int pick = menu_select("Approve action?  " + ev.text,
                            {"Deny", "Allow once", "Allow for this session"});
    agent::Approval d = agent::Approval::Deny;
    if (pick == 1) d = agent::Approval::AllowOnce;
    else if (pick == 2) d = agent::Approval::AllowSession;
    const char* verdict = "allowed for session";
    if (d == agent::Approval::Deny) verdict = "denied";
    else if (d == agent::Approval::AllowOnce) verdict = "allowed once";
    append_line(P_STATUS,
                std::string("approval: ") + verdict + "  (" + ev.text + ")");
    if (ev.approval_promise)
        ev.approval_promise->set_value(d);
}

void Tui::pump_events() {
    if (drain_events()) {
        draw();
        flush();
    }
}

void Tui::send_async(const std::string& prompt) {
    if (agent_busy_.load()) return;

    // If the previous agent thread has finished but wasn't joined (because
    // agent_busy_ went false before the thread handle was cleaned up), join
    // it now so the next std::thread assignment doesn't call terminate().
    if (agent_thread_.joinable())
        agent_thread_.join();

    agent_busy_.store(true);
    agent_cancel_.store(false);

    ensure_chat_window();
    append_line(P_USER, "> " + prompt);

    auto& w = win();
    w.reason_buf.clear();
    w.reason_folded = false;
    show_reasoning_ = cfg_.show_reasoning;
    w.stream_ts = timestamp();

    agent_thread_ = std::thread([this, prompt] { agent_worker(prompt); });
}

void Tui::agent_worker(const std::string& prompt) {
    agent::AgentHooks hooks;

    hooks.on_reasoning = [this](const std::string& d) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Reasoning;
        ev.text = d;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_token = [this](const std::string& d) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Token;
        ev.text = d;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_state = [this](agent::RunState s) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::StateChange;
        ev.state = s;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_stats = [this](const agent::Stats& s) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Stats;
        ev.stats = s;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_status = [this](const std::string& s) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Status;
        ev.text = s;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_tool_call = [this](const std::string& n, const agent::json& a) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::ToolCall;
        ev.tool_name = n;
        ev.tool_args = a;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_tool_result = [this](const std::string& n, const agent::ToolResult& r) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::ToolResult;
        ev.tool_name = n;
        ev.tool_result = r;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_assistant = [this](const std::string& s) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Assistant;
        ev.text = s;
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_approval = [this](const std::string& name,
                               const agent::json& args,
                               const std::string& summary) -> agent::Approval {
        if (agent_cancel_.load()) return agent::Approval::Deny;
        auto p = std::make_shared<std::promise<agent::Approval>>();
        auto f = p->get_future();
        {
            AgentEvent ev;
            ev.type = AgentEvent::Approval;
            ev.text = summary;
            ev.approval_promise = p;
            std::scoped_lock lk(event_mtx_);
            event_queue_.push(std::move(ev));
        }
        (void)name;
        (void)args;
        return f.get();
    };

    try {
        if (!agent_cancel_.load()) {
            win().agent->set_hooks(hooks);
            win().agent->run(prompt);
            win().dirty = true;
        }
    } catch (const std::exception& e) {
        AgentEvent ev;
        ev.type = AgentEvent::Error;
        ev.error_msg = e.what();
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(ev));
    }

    AgentEvent done;
    done.type = AgentEvent::Done;
    {
        std::scoped_lock lk(event_mtx_);
        event_queue_.push(std::move(done));
    }
    agent_busy_.store(false);
}

void Tui::run() {
    draw();
    draw_input("");
    flush();
    detect_server(false);

    timeout(50);

    std::string input;
    while (!quit_) {
        bool had_events = drain_events();

        // Reap background jobs that hit their idle/hard deadline. Runs every
        // tick so long-running processes are auto-killed even while the user is
        // idle and no agent event is flowing.
        jobs_.check_timeouts();

        int ch = getch();
        if (ch == ERR) {
            bool repainted = had_events;
            if (had_events) {
                draw();
            } else if (agent_busy_.load() || jobs_.running_count() > 0) {
                // Either the agent is blocked on a call that emits no streaming
                // tokens (silent confirmation exchange, a non-streaming tool
                // result, or the model simply thinking), or a background job is
                // running. In both cases no AgentEvent flows every frame, so this
                // branch is what keeps the clock, the progress wave, and the
                // background-job countdown alive. Repaint the status bar on a
                // fixed wall-clock cadence instead of gating on anim_phase_
                // (which would lock at one phase and freeze the whole bar).
                auto now = std::chrono::steady_clock::now();
                if (now - last_status_tick_ > std::chrono::milliseconds(150)) {
                    last_status_tick_ = now;
                    tick_clock();
                    repainted = true;
                }
            }
            // Any background repaint (streamed tokens or the status tick) moves
            // the virtual cursor off the input line; park it back so the caret
            // stays put instead of jumping to the status bar while the agent runs.
            if (repainted || input != last_input_) {
                draw_input(input);
                last_input_ = input;
            }
            if (!agent_busy_.load() && !pending_prompt_.empty()) {
                std::string p = std::move(pending_prompt_);
                send_async(p);
            }
            if (dirty_) flush();
            continue;
        }
        // Alt+1..9 window switch, sent as a single meta-encoded key by some
        // terminals (0x80 | digit). Works even while the agent is busy.
        if (ch >= 0xB1 && ch <= 0xB9) {
            switch_to(static_cast<size_t>(ch - 0xB1));
            draw_input(input);
            continue;
        }

        if (ch == 14) {
            if (agent_busy_.load()) continue;
            new_window("chat");
            draw(); draw_input(input); continue;
        }
        if (ch == 23) {
            if (agent_busy_.load()) continue;
            close_window();
            draw(); draw_input(input); continue;
        }
        if (ch == 27) {
            if (drawer_open_) {
                drawer_open_ = false;
                draw(); draw_input(input);
                continue;
            }
            // Alt+number arrives as ESC followed by a digit. Read the next key
            // first so the chord switches windows even while the agent is busy;
            // a lone ESC is the fallback that cancels a running agent.
            int n = getch();
            if (n >= '1' && n <= '9') {
                switch_to(static_cast<size_t>(n - '1'));
                draw_input(input);
                continue;
            }
            if (agent_busy_.load()) {
                agent_cancel_.store(true);
                continue;
            }
            continue;
        }
        if (ch == '\t') {
            // Argument context (/cmd partial): complete against the command's
            // own candidate list (zsh-style). A single Tab extends the input to
            // the shared prefix inline; only a second Tab on an already-complete
            // (ambiguous) prefix opens the ncurses selection popup.
            if (input.find(' ') != std::string::npos) {
                size_t sp = input.find(' ');
                std::string name = input.substr(1, sp - 1);
                const Command* c = find_command(name);
                if (c && c->complete_arg) {
                    std::string partial = input.substr(sp + 1);
                    auto choices = c->complete_arg(partial);
                    if (!choices.empty()) {
                        if (choices.size() == 1) {
                            input = "/" + name + " " + choices[0] + " ";
                            arg_tab_prefix_.clear();
                        } else {
                            std::string cp = palette::common_prefix(choices);
                            if (cp.size() > partial.size()) {
                                // Single Tab: extend inline to the shared
                                // prefix. A following Tab (when already there)
                                // opens the selection popup.
                                input = std::string{"/"}.append(name).append(" ").append(cp);
                                arg_tab_prefix_ = input;
                            } else if (input == arg_tab_prefix_) {
                                // Second Tab on an already-complete prefix:
                                // show the zsh-style selection list.
                                int sel = menu_select("complete: " + input,
                                                      choices);
                                if (sel >= 0 &&
                                    sel < static_cast<int>(choices.size()))
                                    input = "/" + name + " " + choices[sel] +
                                            " ";
                                arg_tab_prefix_.clear();
                            } else {
                                // First Tab at the prefix: nothing more to
                                // extend inline; arm for a second Tab.
                                arg_tab_prefix_ = input;
                            }
                        }
                        if (drawer_open_) { drawer_open_ = false; draw(); }
                        draw_input(input);
                        continue;
                    }
                }
            }
            // Command-name completion via the drawer.
            if (drawer_open_) {
                input = drawer_complete(input);
                drawer_sel_ = 0;
                draw(); draw_input(input);
                continue;
            }
        }
        if (drawer_open_ && (ch == KEY_UP || ch == KEY_DOWN)) {
            int n = static_cast<int>(filter_commands(drawer_token(input)).size());
            if (n > 0) {
                if (ch == KEY_DOWN) drawer_sel_ = (drawer_sel_ + 1) % n;
                else drawer_sel_ = (drawer_sel_ + n - 1) % n;
            }
            draw_input(input);
            continue;
        }
        if (ch == KEY_NPAGE) {
            if (win().read_only) continue;
            win().scroll_top = std::min(max_scroll(),
                                   win().scroll_top + lines_per_page());
            draw(); draw_input(input); continue;
        }
        if (ch == KEY_PPAGE) {
            if (win().read_only) continue;
            win().scroll_top = std::max(0, win().scroll_top - lines_per_page());
            draw(); draw_input(input); continue;
        }
        if (ch == 7 || ch == 10 || ch == 13 || ch == KEY_ENTER) {
            if (drawer_open_ && !drawer_has_arg(input)) {
                auto matches = filter_commands(drawer_token(input));
                if (!matches.empty() &&
                    drawer_sel_ < static_cast<int>(matches.size())) {
                    const Command* c = matches[drawer_sel_];
                    drawer_open_ = false;
                    input.clear();
                    handle_slash("/" + c->name);
                    draw(); draw_input("");
                    continue;
                }
            }
            if (input.empty()) continue;
            std::string prompt = input;
            drawer_open_ = false;
            if (handle_slash(prompt)) {
                input.clear();
                draw(); draw_input("");
                continue;
            }
            if (agent_busy_.load()) {
                // Agent is mid-turn: keep the prompt and send it automatically
                // once the agent returns to idle, so typing never feels blocked.
                pending_prompt_ = prompt;
                input.clear();
                append_line(P_STATUS,
                            "queued: will send when the agent is idle");
                draw(); draw_input(input);
                continue;
            }
            ensure_chat_window();
            input.clear();
            draw();
            send_async(prompt);
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (!input.empty()) input.pop_back();
            arg_tab_prefix_.clear();
            update_drawer(input);
            draw(); draw_input(input);
        } else if (ch >= 32 && ch <= 126) {
            input += static_cast<char>(ch);
            arg_tab_prefix_.clear();
            update_drawer(input);
            ensure_chat_window();
            draw(); draw_input(input);
        }
        if (dirty_) {
            flush();
            dirty_ = false;
        }
    }
}

} // namespace tui

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

Tui::Tui(agent::Config cfg, agent::ToolRegistry& reg)
    : cfg_(std::move(cfg)), reg_(reg), store_() {
    std::setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(1);
    start_color();
    use_default_colors();
    use_legacy_coding(1);
    init_pairs();
    open_welcome_window();
}

Tui::~Tui() {
    agent_cancel_ = true;
    if (agent_thread_.joinable()) agent_thread_.join();

    for (size_t i = 0; i < windows_.size(); ++i) {
        Window& w = *windows_[i];
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
        std::lock_guard<std::mutex> lk(event_mtx_);
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
        case AgentEvent::ToolCall:
            flush_stream();
            append_line(P_STATUS, "tool: " + ev.tool_name + " " + ev.tool_args.dump());
            break;
        case AgentEvent::ToolResult:
            append_line(P_STATUS,
                        "result:" + ev.tool_name + " " +
                        (ev.tool_result.ok ? ev.tool_result.output
                                           : ev.tool_result.error));
            break;
        case AgentEvent::Assistant:
            if (win().stream_buf.empty())
                append_line(P_ASSISTANT, ev.text);
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
            flush_stream();
            autosave();
            win().dirty = true;
            break;
        case AgentEvent::Approval: {
            int pick = menu_select("Approve action?  " + ev.text,
                                   {"Deny", "Allow once", "Allow for this session"});
            agent::Approval d = agent::Approval::Deny;
            if (pick == 1) d = agent::Approval::AllowOnce;
            else if (pick == 2) d = agent::Approval::AllowSession;
            append_line(P_STATUS,
                        std::string("approval: ") +
                        (d == agent::Approval::Deny ? "denied" :
                         d == agent::Approval::AllowOnce ? "allowed once" :
                         "allowed for session") + "  (" + ev.text + ")");
            if (ev.approval_promise)
                ev.approval_promise->set_value(d);
            break;
            }
        }
    }
    return true;
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
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_token = [this](const std::string& d) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Token;
        ev.text = d;
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_state = [this](agent::RunState s) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::StateChange;
        ev.state = s;
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_stats = [this](const agent::Stats& s) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Stats;
        ev.stats = s;
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_status = [this](const std::string& s) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Status;
        ev.text = s;
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_tool_call = [this](const std::string& n, const agent::json& a) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::ToolCall;
        ev.tool_name = n;
        ev.tool_args = a;
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_tool_result = [this](const std::string& n, const agent::ToolResult& r) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::ToolResult;
        ev.tool_name = n;
        ev.tool_result = r;
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(ev));
    };
    hooks.on_assistant = [this](const std::string& s) {
        if (agent_cancel_.load()) return;
        AgentEvent ev;
        ev.type = AgentEvent::Assistant;
        ev.text = s;
        std::lock_guard<std::mutex> lk(event_mtx_);
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
            std::lock_guard<std::mutex> lk(event_mtx_);
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
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(ev));
    }

    AgentEvent done;
    done.type = AgentEvent::Done;
    {
        std::lock_guard<std::mutex> lk(event_mtx_);
        event_queue_.push(std::move(done));
    }
    agent_busy_.store(false);
}

void Tui::run() {
    draw();
    draw_input("");
    detect_server(false);

    timeout(50);

    std::string input;
    while (!quit_) {
        bool had_events = drain_events();

        int ch = getch();
        if (ch == ERR) {
            if (had_events) {
                draw();
            } else {
                tick_clock();
            }
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
            if (agent_busy_.load()) {
                agent_cancel_.store(true);
                continue;
            }
            int n = getch();
            if (n >= '1' && n <= '9') {
                switch_to(static_cast<size_t>(n - '1'));
                draw_input(input);
            }
            continue;
        }
        if (ch == '\t' && drawer_open_) {
            input = drawer_complete(input);
            drawer_sel_ = 0;
            draw(); draw_input(input);
            continue;
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
            if (agent_busy_.load()) continue;
            input.clear();
            draw();
            send_async(prompt);
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (!input.empty()) input.pop_back();
            update_drawer(input);
            draw(); draw_input(input);
        } else if (ch >= 32 && ch <= 126) {
            input += static_cast<char>(ch);
            update_drawer(input);
            draw(); draw_input(input);
        }
    }
}

} // namespace tui

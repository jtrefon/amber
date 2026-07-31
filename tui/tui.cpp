// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "command_line.h"
#include "confirm_panel.h"

#include <agent.h>
#include <agent/mcp_tools.h>
#include <agent/compressor.h>
#include <agent/experience.h>
#include <agent/tools.h>

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

// Signal handler saves workspace before the process dies (SIGHUP/SIGTERM).
// The pointer is set once in the Tui constructor and cleared in the destructor.
static Tui* signal_tui_instance = nullptr;
static void signal_handler(int sig) {
    (void)sig;
    if (signal_tui_instance)
        signal_tui_instance->save_workspace_now();
    _Exit(1);
}

Tui::Tui(agent::Config cfg, agent::ToolRegistry& reg, agent::JobService& jobs)
    : cfg_(std::move(cfg)), reg_(reg), jobs_(jobs),
      mcp_servers_(agent::load_mcp_servers(), &this->cfg_.cancel_token),
      settings_path_(agent::Workspace::local_dir() + "/settings") {
    std::setlocale(LC_ALL, "");
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

    // Signal handler ensures workspace is saved on terminal close / kill.
    signal_tui_instance = this;
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
    signal_tui_instance = nullptr;
    std::fputs("\033[?1007l", stdout);
    std::fflush(stdout);

    save_workspace_now();

    agent_cancel_ = true;
    if (agent_thread_.joinable()) agent_thread_.join();

    for (const auto & window : windows_) {
        Window& w = *window;
        if (!w.dirty || !w.agent || w.agent->context().get_all().empty()) continue;
        std::fprintf(stderr, "\rsaving session '%s'...", w.title.c_str());
        std::fflush(stderr);
        agent::Session s = snapshot(w);
        if (store_.save(s)) w.session_id = s.id;
    }
    std::fprintf(stderr, "\rsession save complete\n");
    endwin();
}

Window& Tui::new_window(const std::string& title) {
    auto w = std::make_unique<Window>();
    w->title = title;
    auto comp_cfg = agent::load_compression_config(cfg_);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto exp_cfg = agent::load_experience_config(cfg_);
    auto mem_store = agent::make_memory_store(exp_cfg);
    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem_store);
    w->agent = std::make_unique<agent::Agent>(cfg_, reg_,
        agent::AgentHooks{},
        std::move(compressor), std::move(gate),
        std::move(mem_store), std::move(retriever));
    w->agent->policy().init(agent::Workspace::local_dir() + "/policy.json");
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
            live_ctx_offset_ += (static_cast<long>(ev.text.size()) / 4) + 1;
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
            // Tool may have modified files — refresh git state for prompt.
            git_refresh();
            break;
        }
        case AgentEvent::Assistant:
            if (win().stream_buf.empty())
                append_markdown(ev.text);
            break;
        case AgentEvent::Stats:
            stats_ = ev.stats;
            if (ev.stats.prompt_tokens >= 0) {
                ctx_used_ = ev.stats.prompt_tokens;
                live_ctx_offset_ = 0;
            }
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
        case AgentEvent::CompressResult: {
            auto& r = ev.compress_result;
            state_ = agent::RunState::Idle;
            ctx_used_ = static_cast<long>(r.tokens_after);
            if (r.messages_before == 0) {
                append_line(P_STATUS, "compress: no compressor configured");
            } else if (r.messages_after >= r.messages_before) {
                append_line(P_STATUS, "compress: nothing to prune ("
                            + std::to_string(r.messages_before)
                            + " messages, ~" + std::to_string(r.tokens_before)
                            + " tokens)");
            } else {
                append_line(P_STATUS,
                    "compress: " + std::to_string(r.messages_before)
                    + " \u2192 " + std::to_string(r.messages_after)
                    + " msgs, ~" + std::to_string(r.tokens_before)
                    + " \u2192 ~" + std::to_string(r.tokens_after) + " tokens"
                    + "  (core:" + std::to_string(r.core_count)
                    + " ctx:" + std::to_string(r.context_count)
                    + " prune:" + std::to_string(r.prune_count) + ")");
            }
            win().dirty = true;
            break;
        }
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
    agent::Approval d = approve_dialog(ev.text, 60, 0);
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

    agent_busy_.store(true);
    agent_cancel_.store(false);

    ensure_chat_window();
    append_line(P_USER, "> " + raw_prompt);

    auto& w = win();
    w.reason_buf.clear();
    w.reason_folded = false;
    show_reasoning_ = cfg_.show_reasoning;
    w.stream_ts = timestamp();

    std::string prompt = expand_at_references(raw_prompt);

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

    // Subscribe to context change events for live token count updates.
    win().agent->context_events().subscribe(
        [this](size_t tokens, size_t) {
            ctx_used_ = static_cast<long>(tokens);
        });

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

void Tui::compress_worker() {
    // Run on a background thread so the main event loop keeps processing
    // events (status messages, input) during the long LLM calls.
    agent_busy_.store(true);
    std::thread t([this]() {
        AgentEvent ev;
        ev.type = AgentEvent::CompressResult;
        if (win().agent) {
            ev.compress_result = win().agent->compress_now();
        }
        agent_busy_.store(false);
        {
            std::scoped_lock lk(event_mtx_);
            event_queue_.push(std::move(ev));
        }
    });
    t.detach();
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

    std::string stat = read_stdout("git diff --shortstat 2>/dev/null");
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

void Tui::run() {
    draw();
    draw_input("");
    flush();
    detect_server(false);
    timeout(50);

    // Build the setting registry and command tree.
    build_settings();
    (void)commands();  // force command tree build
    // Load completion metadata from JSON (help text, choices, ranges).
    // This is the single source of truth for completion metadata — code edits
    // cannot break completion unless the JSON file is damaged.
    // Search order: binary dir, workspace root, user config dir.
    {
        auto try_load = [&](const std::string& path) {
            if (path.empty()) return false;
            bool ok = settings_.load_completions_json(path);
            if (ok) append_line(P_DEBUG, "loaded completions from " + path);
            return ok;
        };
        // 1. Next to the amber binary (/proc/self/exe).
        std::string bin_dir;
        {
            std::array<char, 4096> buf;
            ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
            if (len > 0) {
                buf[len] = '\0';
                std::string exe(buf.data());
                size_t slash = exe.rfind('/');
                if (slash != std::string::npos) {
                    bin_dir = exe.substr(0, slash);
                    if (try_load(bin_dir + "/completions.json"))
                        goto json_loaded;
                }
            }
        }
        // 2. Workspace root (project directory).
        {
            std::string ws = agent::Workspace::root();
            if (!ws.empty() && try_load(ws + "/completions.json"))
                goto json_loaded;
        }
        // 3. User config directory.
        {
            const char* home = std::getenv("HOME");
            if (home)
                try_load(std::string(home) + "/.config/amber/completions.json");
        }
        json_loaded:;
    }

    // CommandLine is pure logic (no ncurses) and fully tested via e2e tests.
    CommandLine cl;
    cl.set_history(win().prompt_history);

    // Helper: update CommandLine's completion context from the command tree and JSON.
    // This is the SINGLE source of completions — no duplicate logic in draw_drawer.
    auto update_completions = [&]() {
        std::string input = cl.text();
        size_t sp = input.find(' ');
        if (sp != std::string::npos && input[0] == '/') {
            std::string cmd_name = input.substr(1, sp - 1);
            std::string partial = input.substr(sp + 1);

            // 1. SettingRegistry for /get and /set (dotted key config).
            if (cmd_name == "get" || cmd_name == "set") {
                // Convert space-separated partial to dotted for registry lookup:
                // "policy mode" → "policy.mode"
                std::string dotted_partial;
                size_t p = 0;
                while (p < partial.size()) {
                    size_t spc = partial.find(' ', p);
                    if (spc == std::string::npos) { dotted_partial += partial.substr(p); break; }
                    if (!dotted_partial.empty()) dotted_partial += ".";
                    dotted_partial += partial.substr(p, spc - p);
                    p = spc + 1;
                }
                // Strip the last namespace level so we complete leaf names only.
                // e.g. "policy.mo" → ns="policy", leaf="mo" → we want completions for "policy"
                std::string ns_part, leaf_part;
                size_t last_dot = dotted_partial.rfind('.');
                if (last_dot != std::string::npos) {
                    ns_part = dotted_partial.substr(0, last_dot);
                    leaf_part = dotted_partial.substr(last_dot + 1);
                } else {
                    leaf_part = dotted_partial;
                }
                auto completions = settings_.complete(ns_part.empty() ? leaf_part : ns_part);
                std::vector<std::string> stripped;
                for (auto& c : completions) {
                    // Only show keys that match the leaf_part prefix.
                    if (!leaf_part.empty() && c.rfind(leaf_part, 0) != 0) continue;
                    // Strip the namespace prefix.
                    if (!ns_part.empty()) {
                        if (c.rfind(ns_part + ".", 0) == 0)
                            stripped.push_back(c.substr(ns_part.size() + 1));
                    } else {
                        stripped.push_back(c);
                    }
                }
                // Also include single-level keys from the legacy complete_arg.
                const Command* cmd = find_command(cmd_name);
                if (cmd && cmd->complete_arg) {
                    auto legacy = cmd->complete_arg(partial);
                    for (const auto& l : legacy)
                        if (std::find(stripped.begin(), stripped.end(), l) == stripped.end())
                            stripped.push_back(l);
                }
                cl.set_completions(stripped);
                return;
            }

            // 2. Subcommands from JSON (system, files, provider, model, session, job, window).
            auto subs = settings_.subcommands_for(cmd_name);
            if (!subs.empty()) {
                if (partial.empty()) {
                    cl.set_completions(subs);
                } else {
                    std::vector<std::string> filtered;
                    for (const auto& s : subs)
                        if (s.rfind(partial, 0) == 0)
                            filtered.push_back(s);
                    cl.set_completions(filtered);
                }
                return;
            }

            // 3. Legacy complete_arg lambda (fallback).
            const Command* cmd = find_command(cmd_name);
            if (cmd && cmd->complete_arg) {
                cl.set_completions(cmd->complete_arg(partial));
                return;
            }
        }
        // Default: top-level command names (including aliases).
        std::vector<std::string> names;
        for (const auto& c : commands()) {
            names.push_back(c.name);
            for (const auto& a : c.aliases)
                names.push_back(a);
        }
        cl.set_completions(names);
    };
    update_completions();

    while (!quit_) {
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
                    tick_clock();
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

} // namespace tui

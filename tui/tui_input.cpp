// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace tui {

void Tui::send(const std::string& prompt) {
    agent::AgentHooks hooks;
    win().reason_buf.clear();
    win().reason_folded = false;
    show_reasoning_ = cfg_.show_reasoning;
    win().stream_ts = timestamp();
    hooks.on_reasoning = [this](const std::string& d) {
        win().reason_buf += d;
        win().scroll_top = max_scroll();
        draw();
    };
    hooks.on_token = [this](const std::string& d) {
        if (!win().reason_folded && !win().reason_buf.empty()) {
            fold_reasoning();
        }
        win().stream_color = P_ASSISTANT;
        win().stream_buf += d;
        win().scroll_top = max_scroll();
        draw();
    };
    hooks.on_assistant = [this](const std::string& s) {
        if (win().stream_buf.empty()) append_line(P_ASSISTANT, s);
    };
    hooks.on_status = [this](const std::string& s) { append_line(P_STATUS, s); };
    hooks.on_tool_call = [this](const std::string& n, const agent::json& a) {
        flush_stream();
        append_line(P_STATUS, "tool: " + n + " " + a.dump());
    };
    hooks.on_tool_result = [this](const std::string& n, const agent::ToolResult& r) {
        append_line(P_STATUS, "result:" + n + " " + (r.ok ? r.output : r.error));
    };
    hooks.on_approval = [this](const std::string&, const agent::json&,
                               const std::string& summary) -> agent::Approval {
        flush_stream();
        int pick = menu_select("Approve action?  " + summary,
                               {"Deny", "Allow once", "Allow for this session"});
        agent::Approval d = agent::Approval::Deny;
        if (pick == 1) d = agent::Approval::AllowOnce;
        else if (pick == 2) d = agent::Approval::AllowSession;
        append_line(P_STATUS,
                    std::string("approval: ") +
                    (d == agent::Approval::Deny ? "denied" :
                     d == agent::Approval::AllowOnce ? "allowed once" :
                     "allowed for session") + "  (" + summary + ")");
        draw();
        return d;
    };
    hooks.on_state = [this](agent::RunState s) {
        state_ = s;
        draw();
    };
    hooks.on_stats = [this](const agent::Stats& s) {
        stats_ = s;
        if (s.prompt_tokens >= 0) ctx_used_ = s.prompt_tokens;
        draw();
    };
    try {
        win().agent->set_hooks(hooks);
        win().agent->run(prompt);
        win().dirty = true;
    } catch (const std::exception& e) {
        state_ = agent::RunState::Error;
        flush_stream();
        append_line(P_STATUS, std::string("error: ") + e.what());
    }
    if (state_ != agent::RunState::Error) state_ = agent::RunState::Idle;
    flush_stream();
    autosave();
    draw();
}

void Tui::fold_reasoning() {
    if (win().reason_folded) return;
    win().reason_folded = true;
    if (win().reason_buf.empty()) return;
    size_t words = 1;
    for (char ch : win().reason_buf) if (ch == ' ') ++words;
    append_line_ts(P_REASONING,
                   "[thought for " + std::to_string(words) + " words]",
                   win().stream_ts.empty() ? timestamp() : win().stream_ts);
    win().reason_buf.clear();
}

void Tui::flush_stream() {
    if (!win().reason_folded && !win().reason_buf.empty()) fold_reasoning();
    if (win().stream_buf.empty()) return;
    // Commit the streamed reply through the Markdown renderer so headings,
    // code fences, lists, etc. survive into the scrollback (the live preview
    // in draw() already renders it as Markdown).
    append_markdown(win().stream_buf);
    win().stream_buf.clear();
    win().stream_ts.clear();
    draw();
}

void Tui::toggle_thinking() {
    cfg_.show_reasoning = !cfg_.show_reasoning;
    append_line(P_STATUS, std::string("thinking display: ") +
                              (cfg_.show_reasoning ? "on" : "off"));
    draw();
}

void Tui::cmd_policy(const std::string& arg) {
    auto set_mode = [&](agent::AgentMode m) {
        cfg_.mode = m;
        std::vector<const char*> labels = {"read", "write", "yolo"};
        const char* l = labels[static_cast<int>(m)];
        append_line(P_STATUS, std::string("policy set to ") + l);
        draw();
    };
    if (arg.empty()) {
        std::vector<std::string> choices = {"read  (observation only, safe)",
                                            "write (all tools, approval gated)",
                                            "yolo  (all tools, auto-approve)"};
        int sel = menu_select("Select policy", choices);
        if (sel >= 0 && sel <= 2)
            set_mode(static_cast<agent::AgentMode>(sel));
        return;
    }
    if (arg == "read")      set_mode(agent::AgentMode::Read);
    else if (arg == "write") set_mode(agent::AgentMode::Write);
    else if (arg == "yolo")  set_mode(agent::AgentMode::Yolo);
    else append_line(P_STATUS, "usage: /policy read|write|yolo");
}

void Tui::cmd_toolfold(const std::string& arg) {
    ToolFold f;
    if (arg == "always")      f = ToolFold::Always;
    else if (arg == "auto")   f = ToolFold::Auto;
    else if (arg == "never")  f = ToolFold::Never;
    else {
        append_line(P_STATUS, "usage: /toolfold always|auto|never");
        return;
    }
    tool_fold_ = f;
    append_line(P_STATUS, std::string("tool fold set to ") + arg);
    draw();
}

void Tui::cmd_display(const std::string& arg) {
    if (arg == "markdown" || arg.rfind("markdown ", 0) == 0) {
        std::string v = arg == "markdown" ? "" : arg.substr(9);
        if (v == "on" || v.empty()) {
            win().markdown_on = true;
            append_line(P_STATUS, "markdown rendering: on");
        } else if (v == "off") {
            win().markdown_on = false;
            append_line(P_STATUS, "markdown rendering: off");
        } else {
            append_line(P_STATUS, "usage: /display markdown on|off");
        }
    } else {
        append_line(P_STATUS, "usage: /display markdown on|off");
    }
    draw();
}

const std::vector<Command>& Tui::commands() {
    if (commands_.empty()) build_commands();
    return commands_;
}

void Tui::build_commands() {
    commands_ = {
        {"help", {"?", "h"}, "[command]",
         "list commands, or show detail for one",
         [this](const std::string& a) { cmd_help(a); }},
        {"model", {"settings", "server"}, "",
         "set provider URL, token, model, and context (test connection)",
         [this](const std::string&) { settings_screen(); redraw_after_modal(); }},
        {"config", {"cfg"}, "",
         "show the current configuration",
         [this](const std::string&) { config_screen(); redraw_after_modal(); }},
        {"think", {"reasoning"}, "",
         "toggle live thinking/reasoning display",
         [this](const std::string&) { toggle_thinking(); }},
        {"toolfold", {}, "always|auto|never",
         "how to show tool calls in scrollback",
         [this](const std::string& a) { cmd_toolfold(a); },
         [](const std::string&) {
             return std::vector<std::string>{"always", "auto", "never"};
         },
         [this]() -> std::string {
             switch (tool_fold_) {
                 case ToolFold::Always: return "always";
                 case ToolFold::Auto:   return "auto";
                 case ToolFold::Never:  return "never";
             }
             return "auto";
         }},
        {"policy", {"mode"}, "read|write|yolo",
          "set agent policy: read (safe), write (normal), yolo (trusted)",
          [this](const std::string& a) { cmd_policy(a); },
          [](const std::string&) {
              return std::vector<std::string>{"read", "write", "yolo"};
          },
          [this]() -> std::string {
              switch (cfg_.mode) {
                  case agent::AgentMode::Read:  return "read";
                  case agent::AgentMode::Write: return "write";
                  case agent::AgentMode::Yolo:  return "yolo";
              }
              return "write";
          }},
        {"display", {}, "markdown on|off",
          "toggle Markdown rendering of assistant replies",
          [this](const std::string& a) { cmd_display(a); },
          [](const std::string&) {
              return std::vector<std::string>{"markdown on", "markdown off"};
          },
          [this]() -> std::string {
              return std::string("markdown ") +
                     (win().markdown_on ? "on" : "off");
          }},
        {"new", {}, "",
         "open a new chat window",
         [this](const std::string&) { new_window("chat"); draw(); }},
        {"close", {}, "",
         "close the current window",
         [this](const std::string&) { close_window(); }},
        {"window", {"win", "w"}, "new|close|list|rename <name>",
         "manage chat windows",
         [this](const std::string& a) { cmd_window(a); }},
        {"save", {}, "",
         "persist the current conversation",
         [this](const std::string&) { save_session(); }},
        {"sessions", {"load", "open"}, "",
         "browse and load a saved session",
         [this](const std::string&) { session_browser(); }},
        {"quit", {"exit", "q"}, "",
         "save all windows and exit",
         [this](const std::string&) { request_quit(); }},
    };
}

const Command* Tui::find_command(const std::string& name) {
    return palette::find(commands(), name);
}

bool Tui::handle_slash(const std::string& line) {
    if (line.empty() || line[0] != '/') return false;
    std::string rest = line.substr(1);
    std::string name, arg;
    size_t sp = rest.find(' ');
    if (sp == std::string::npos) name = rest;
    else { name = rest.substr(0, sp); arg = rest.substr(sp + 1); }

    const Command* c = find_command(name);
    if (!c) {
        append_line(P_STATUS,
                    "unknown command: /" + name + "  (try /help)");
        return true;
    }
    // Invoked with no argument but the command expects a fixed option: show a
    // BitchX-style help frame with description, current value and options.
    if (arg.empty() && c->complete_arg && !c->args.empty()) {
        show_command_frame(*c);
        return true;
    }
    c->run(arg);
    return true;
}

std::string Tui::usage(const Command& c) const {
    return palette::usage(c);
}

void Tui::show_command_frame(const Command& c) {
    // Invoked with no argument: report the current setting neutrally (never a
    // modal dialog, so it can't stall the agent worker). Providing a value is a
    // separate path that confirms the change; this is just a status read-out.
    std::string cur = c.current_value ? c.current_value() : "";
    if (!cur.empty())
        append_line(P_STATUS,
                    "/" + c.name + " is currently: " + cur);
    else
        append_line(P_STATUS, "/" + c.name + ": " + c.help);
    if (c.complete_arg) {
        auto opts = c.complete_arg("");
        if (!opts.empty()) {
            std::string line = "  choices:";
            for (auto& o : opts) line += "  " + o;
            append_line(P_STATUS, line);
        }
    }
    draw();
}

void Tui::cmd_help(const std::string& arg) {
    if (arg.empty()) {
        banner("Slash commands (type /help <command> for detail):");
        size_t w = 0;
        for (const auto& c : commands()) w = std::max(w, usage(c).size());
        for (const auto& c : commands()) {
            std::string u = usage(c);
            u.append(w - u.size() + 2, ' ');
            append_line(P_STATUS, "  " + u + c.help);
        }
        append_line(P_STATUS, "");
        append_line(P_STATUS, "Keys:  Enter/Ctrl-G send   PgUp/PgDn scroll");
        append_line(P_STATUS,
                    "       Ctrl-N new window   Ctrl-W close   Alt+1..9 switch");
        append_line(P_STATUS, "       Ctrl-C quit");
        append_line(P_STATUS,
                    "Type '/' to open the command drawer (filter, Tab, Enter).");
        draw();
        return;
    }
    std::string name = arg;
    if (!name.empty() && name[0] == '/') name = name.substr(1);
    const Command* c = find_command(name);
    if (!c) { append_line(P_STATUS, "no such command: /" + name); return; }
    banner(usage(*c));
    append_line(P_STATUS, "  " + c->help);
    if (!c->aliases.empty()) {
        std::string al = "  aliases:";
        for (const auto& a : c->aliases) al += " /" + a;
        append_line(P_STATUS, al);
    }
    draw();
}

void Tui::cmd_window(const std::string& arg) {
    if (arg == "new") { new_window("chat"); draw(); }
    else if (arg == "close") { close_window(); }
    else if (arg == "list") {
        std::string s = "windows:";
        for (size_t i = 0; i < windows_.size(); ++i)
            s += " " + std::to_string(i + 1) + ":" + windows_[i]->title +
                 (i == active_ ? "*" : "");
        append_line(P_STATUS, s);
    } else if (arg.rfind("rename ", 0) == 0) {
        win().title = arg.substr(7);
        append_line(P_STATUS, "renamed window to " + win().title);
        draw();
    } else {
        append_line(P_STATUS, "usage: /window new|close|list|rename <name>");
    }
}

void Tui::config_screen() {
    auto mask = [](const std::string& s) {
        return s.empty() ? std::string("(unset)") : std::string(s.size(), '*');
    };
    info_dialog("Configuration", {
        "api_base:  " + cfg_.api_base,
        "api_key:   " + mask(cfg_.api_key),
        "model:     " + cfg_.model,
        "stream:    " + std::string(cfg_.stream ? "on" : "off"),
        "context:   " + (cfg_.context_size > 0
                             ? std::to_string(cfg_.context_size) + " tokens" +
                                   (cfg_.context_explicit ? "" : " (auto-detected)")
                             : std::string("auto (not detected)")),
        "max_iter:  " + std::to_string(cfg_.max_tool_iterations),
        "system:    " + cfg_.system_prompt_path,
        "tools:     " + cfg_.tools_prompt_path,
    });
}

void Tui::detect_server(bool force) {
    agent::ServerInfo info = agent::apply_server_autodetect(cfg_);
    if (!info.ok) {
        if (force)
            append_line(P_STATUS, "detect: server unreachable at " +
                                      cfg_.api_base);
        return;
    }
    last_detected_ = info;
    std::string note = "detected model=" + cfg_.model +
                       " n_ctx=" + std::to_string(cfg_.context_size);
    if (info.context_train > 0 &&
        info.context_train != cfg_.context_size)
        note += " (max " + std::to_string(info.context_train) + ")";
    append_line(P_STATUS, note);
    draw();
}

bool Tui::test_connection(bool announce) {
    agent::ServerInfo info = agent::apply_server_autodetect(cfg_);
    if (!info.ok) {
        append_line(P_STATUS,
                    "test: no response from " + cfg_.api_base +
                    " (check URL/token and that the server is running)");
        draw();
        return false;
    }
    last_detected_ = info;
    std::string note = "test: OK - " + cfg_.api_base +
                       "  model=" + cfg_.model +
                       " n_ctx=" + std::to_string(cfg_.context_size);
    if (info.context_train > 0 && info.context_train != cfg_.context_size)
        note += " (max " + std::to_string(info.context_train) + ")";
    append_line(P_STATUS, note);
    (void)announce;
    draw();
    return true;
}

void Tui::settings_screen() {
    std::string det = last_detected_.ok
        ? ("detected: " + last_detected_.model + " / n_ctx " +
           std::to_string(last_detected_.context_size))
        : std::string("detected: (none - press Detect below)");

    std::vector<std::string> pre = {"Edit settings",
                                    "Test connection & fill model/context"};
    int pick = menu_select(det, pre);
    if (pick == 1) { test_connection(true); return; }
    if (pick != 0) return;

    std::string model_field = cfg_.model_explicit ? cfg_.model : "";
    std::string ctx_field =
        cfg_.context_explicit ? std::to_string(cfg_.context_size) : "0";
    std::vector<FieldSpec> fields = {
        {"Server URL", cfg_.api_base, false},
        {"Token", cfg_.api_key, true},
        {"Model (blank = auto)", model_field, false},
        {"Context n_ctx (0 = auto)", ctx_field, false},
    };
    if (!form_edit("Server settings", fields)) return;

    cfg_.api_base = fields[0].value;
    cfg_.api_key = fields[1].value;
    if (fields[2].value.empty()) {
        cfg_.model_explicit = false;
    } else {
        cfg_.model = fields[2].value;
        cfg_.model_explicit = true;
    }
    try {
        int n = std::stoi(fields[3].value);
        if (n > 0) { cfg_.context_size = n; cfg_.context_explicit = true; }
        else       { cfg_.context_explicit = false; }
    } catch (...) { cfg_.context_explicit = false; }

    if (!cfg_.model_explicit || !cfg_.context_explicit)
        test_connection(false);

    std::vector<std::string> post = {"Save to " + settings_path_,
                                     "Apply only (don't save)"};
    int choice = menu_select("Apply settings?", post);
    if (choice == 0) {
        save_settings();
        append_line(P_STATUS, "settings saved to " + settings_path_);
    } else if (choice == 1) {
        append_line(P_STATUS, "settings applied (not saved)");
    }
}

void Tui::save_settings() { cfg_.save(settings_path_); }

} // namespace tui

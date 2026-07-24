// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "tui/list_panel.h"
#include "agent/model_probe.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

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
        live_ctx_offset_ += static_cast<long>(d.size()) / 4 + 1;
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
        const char* verdict = "allowed for session";
        if (d == agent::Approval::Deny) verdict = "denied";
        else if (d == agent::Approval::AllowOnce) verdict = "allowed once";
        append_line(P_STATUS,
                    std::string("approval: ") + verdict + "  (" + summary + ")");
        draw();
        return d;
    };
    hooks.on_state = [this](agent::RunState s) {
        state_ = s;
        draw();
    };
    hooks.on_stats = [this](const agent::Stats& s) {
        stats_ = s;
        if (s.prompt_tokens >= 0) {
            ctx_used_ = s.prompt_tokens;
            live_ctx_offset_ = 0;
        }
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

void Tui::cmd_set(const std::string& arg) {
    if (arg.empty()) { append_line(P_STATUS, "usage: /set <option> <value>"); return; }

    // Parse: /set detection loop off
    size_t sp = arg.find(' ');
    std::string ns = (sp == std::string::npos) ? arg : arg.substr(0, sp);
    std::string rest = (sp == std::string::npos) ? "" : arg.substr(sp + 1);

    // detection loop|duplicate off|on|toggle
    if (ns == "detection") {
        size_t sp2 = rest.find(' ');
        std::string key = (sp2 == std::string::npos) ? "" : rest.substr(0, sp2);
        std::string val = (sp2 == std::string::npos) ? "" : rest.substr(sp2 + 1);
        if (key != "loop" && key != "duplicate") {
            append_line(P_STATUS, "usage: /set detection loop|duplicate off|on|toggle");
            return;
        }
        if (val != "off" && val != "on" && val != "toggle") {
            append_line(P_STATUS, "usage: /set detection " + key + " off|on|toggle (got: " + val + ")");
            return;
        }
        bool* field = (key == "loop") ? &cfg_.detection_loop : &cfg_.detection_duplicate;
        bool new_val = (val == "on") ? true : (val == "off") ? false : !*field;
        *field = new_val;
        for (auto& w : windows_) {
            if (!w->agent) continue;
            if (key == "loop") w->agent->set_detection_loop(new_val);
            else w->agent->set_detection_duplicate(new_val);
        }
        std::string hint = (key == "loop")
            ? (new_val ? "breaks on repeat" : "runs until stop")
            : (new_val ? "rejects duplicates" : "may repeat calls");
        append_line(P_STATUS, "detection " + key + ": " + (new_val ? "on" : "off") + " — " + hint);
        if (!cfg_.save_settings(settings_path_))
            append_line(P_STATUS, "warning: could not save to " + settings_path_);
        draw();
        return;
    }

    // display markdown on|off
    if (ns == "display") {
        if (rest != "markdown on" && rest != "markdown off") {
            append_line(P_STATUS, "usage: /set display markdown on|off");
            return;
        }
        win().markdown_on = (rest == "markdown on");
        append_line(P_STATUS, "markdown rendering: " + std::string(rest.substr(9)));
        draw();
        return;
    }

    // toolfold always|auto|never
    if (ns == "toolfold") {
        if (rest == "always") tool_fold_ = ToolFold::Always;
        else if (rest == "auto") tool_fold_ = ToolFold::Auto;
        else if (rest == "never") tool_fold_ = ToolFold::Never;
        else { append_line(P_STATUS, "usage: /set toolfold always|auto|never"); return; }
        append_line(P_STATUS, "tool fold: " + rest);
        draw();
        return;
    }

    // policy read|write|yolo
    if (ns == "policy") {
        if (rest == "read") cfg_.mode = agent::AgentMode::Read;
        else if (rest == "write") cfg_.mode = agent::AgentMode::Write;
        else if (rest == "yolo") cfg_.mode = agent::AgentMode::Yolo;
        else { append_line(P_STATUS, "usage: /set policy read|write|yolo"); return; }
        append_line(P_STATUS, "policy: " + rest);
        draw();
        return;
    }

    // provider openrouter|kilocode|custom
    if (ns == "provider") {
        cmd_provider(rest);
        return;
    }

    // model <name>
    if (ns == "model") {
        cmd_model(rest);
        return;
    }

    // think on|off|auto
    if (ns == "think") {
        if (rest != "on" && rest != "off" && rest != "auto") {
            append_line(P_STATUS, "usage: /set think on|off|auto");
            return;
        }
        cfg_.thinking = rest;
        append_line(P_STATUS, "thinking: " + rest);
        return;
    }

    append_line(P_STATUS, "unknown option: " + ns + " (try: detection, display, toolfold, policy, provider, model, think)");
}

void Tui::cmd_get(const std::string& arg) {
    if (arg == "config" || arg.empty()) {
        config_screen();
        redraw_after_modal();
        return;
    }
    if (arg == "model") {
        append_line(P_STATUS, "model: " + cfg_.model + " (provider: " + cfg_.provider_name + ")");
        return;
    }
    if (arg == "provider") {
        append_line(P_STATUS, "provider: " + cfg_.provider_name + " (" + cfg_.api_base + ")");
        return;
    }
    if (arg == "toolfold") {
        std::string v = (tool_fold_ == ToolFold::Always) ? "always" :
                        (tool_fold_ == ToolFold::Never) ? "never" : "auto";
        append_line(P_STATUS, "toolfold: " + v);
        return;
    }
    if (arg == "policy") {
        std::string v = (cfg_.mode == agent::AgentMode::Read) ? "read" :
                        (cfg_.mode == agent::AgentMode::Yolo) ? "yolo" : "write";
        append_line(P_STATUS, "policy: " + v);
        return;
    }
    if (arg == "display") {
        append_line(P_STATUS, "markdown: " + std::string(win().markdown_on ? "on" : "off"));
        return;
    }
    if (arg == "think") {
        append_line(P_STATUS, "thinking: " + cfg_.thinking);
        return;
    }
    if (arg.rfind("detection", 0) == 0) {
        std::string sub = (arg.size() > 10) ? arg.substr(10) : "";
        if (sub.empty() || sub == " loop") {
            append_line(P_STATUS, "detection loop: " + std::string(cfg_.detection_loop ? "on" : "off"));
        }
        if (sub.empty() || sub == " duplicate") {
            append_line(P_STATUS, "detection duplicate: " + std::string(cfg_.detection_duplicate ? "on" : "off"));
        }
        return;
    }
    // Default: show all settings via config screen
    config_screen();
    redraw_after_modal();
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
        {"settings", {"server", "endpoint"}, "",
         "configure provider, model, API key, and connection test",
         [this](const std::string&) { settings_screen(); redraw_after_modal(); }},
        {"new", {}, "",
         "open a new chat window",
         [this](const std::string&) { new_window("chat"); draw(); }},
        {"close", {}, "",
         "close the current window",
         [this](const std::string&) { close_window(); }},
        {"window", {"win", "w"}, "new|close|list|rename <name>",
         "manage chat windows",
         [this](const std::string& a) { cmd_window(a); }},
        {"stop", {"cancel", "kill"}, "",
         "terminate the current tool and agent loop",
          [this](const std::string&) {
              cfg_.cancel_token.request();
              agent_cancel_.store(true);
              append_line(P_STATUS, "stop requested");
          }},
        {"set", {}, "<option> <value>",
         "set runtime options: detection, display, toolfold, policy, provider, model, think",
          [this](const std::string& a) { cmd_set(a); },
          [](const std::string& partial) {
              std::vector<std::string> categories = {"detection", "display",
                  "toolfold", "policy", "provider", "model", "think"};
              if (partial.empty()) return categories;
              std::vector<std::string> out;
              for (auto& c : categories)
                  if (c.rfind(partial, 0) == 0) out.push_back(c);
              return out;
          },
          [this]() -> std::string {
              return "detection " + std::string(cfg_.detection_loop ? "on" : "off") +
                     "  display " + (win().markdown_on ? "on" : "off") +
                     "  toolfold " + (tool_fold_ == ToolFold::Always ? "always" :
                                      tool_fold_ == ToolFold::Never ? "never" : "auto") +
                     "  policy " + (cfg_.mode == agent::AgentMode::Read ? "read" :
                                    cfg_.mode == agent::AgentMode::Yolo ? "yolo" : "write") +
                     "  provider " + cfg_.provider_name +
                     "  model " + cfg_.model +
                     "  think " + cfg_.thinking;
          }},
        {"get", {}, "<option>",
         "show current setting: config, model, provider, toolfold, policy, display, detection",
          [this](const std::string& a) { cmd_get(a); },
          [](const std::string& partial) {
              std::vector<std::string> keys = {"config", "model", "provider",
                  "toolfold", "policy", "display", "detection loop", "detection duplicate", "think"};
              if (partial.empty()) return keys;
              std::vector<std::string> out;
              for (auto& k : keys)
                  if (k.rfind(partial, 0) == 0) out.push_back(k);
              return out;
          }},
        {"compress", {"compact"}, "",
         "compress conversation history to free context space",
          [this](const std::string&) { cmd_compress(""); }},
        {"job", {}, "[ls|kill <id>|read <id>|start <cmd>]",
         "manage background processes (servers, builds) started by the agent",
          [this](const std::string& a) { cmd_job(a); },
          [this](const std::string& partial) {
              std::string sub, rest;
              size_t sp = partial.find(' ');
              if (sp == std::string::npos) { sub = partial; rest.clear(); }
              else { sub = partial.substr(0, sp); rest = partial.substr(sp + 1); }
              if (sub.empty())
                  return std::vector<std::string>{"ls", "kill", "read", "start"};
              if (sub == "kill") {
                  std::vector<std::string> out;
                  for (const auto& j : jobs_.list())
                      if ((j.state == agent::JobState::Running ||
                           j.state == agent::JobState::Starting) &&
                          (rest.empty() || j.id.rfind(rest, 0) == 0))
                          out.push_back("kill " + j.id);
                  return out;
              }
              if (sub == "read") {
                  std::vector<std::string> out;
                  for (const auto& j : jobs_.list())
                      if (rest.empty() || j.id.rfind(rest, 0) == 0)
                          out.push_back("read " + j.id);
                  return out;
              }
              return std::vector<std::string>{};
          },
         [this]() -> std::string {
             return std::to_string(jobs_.running_count()) + " running";
         }},
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

void Tui::cmd_model(const std::string& arg) {
    if (arg.empty()) {
        // List available models from the provider
        append_line(P_STATUS, "querying " + cfg_.models_url() + " ...");
        draw();
        auto models = agent::list_models(cfg_);
        if (models.empty()) {
            append_line(P_STATUS, "no models available or server unreachable");
            return;
        }
        append_line(P_STATUS, "available models (" + std::to_string(models.size()) + "):");
        int count = 0;
        for (const auto& m : models) {
            if (count >= 20) {
                append_line(P_STATUS, "  ... and " + std::to_string(models.size() - 20) + " more");
                break;
            }
            bool cur = (m == cfg_.model);
            append_line(P_STATUS, std::string(cur ? "> " : "  ") + m);
            ++count;
        }
        return;
    }

    // Set model by name
    auto models = agent::list_models(cfg_);
    bool found = false;
    for (const auto& m : models)
        if (m == arg) { found = true; break; }
    if (!found) {
        append_line(P_STATUS, "model \"" + arg + "\" not found in provider's model list");
        return;
    }
    cfg_.model = arg;
    cfg_.model_explicit = true;
    // Propagate to window agents
    for (auto& w : windows_) {
        if (!w->agent) continue;
        w->agent->set_detection_loop(cfg_.detection_loop);  // force agent re-init
    }
    std::string global = agent::global_config_path();
    cfg_.save_global(global);
    append_line(P_STATUS, "model set to " + arg + " (saved to " + global + ")");
}

void Tui::cmd_provider(const std::string& arg) {
    if (arg.empty()) {
        append_line(P_STATUS, "current provider: " + cfg_.provider_name +
                     " (" + cfg_.api_base + ")");
        return;
    }
    auto* prov = agent::provider::find(arg);
    if (!prov || prov->name == "custom" && arg != "custom") {
        append_line(P_STATUS, "unknown provider: " + arg +
                     " (try: openrouter, kilocode, custom)");
        return;
    }
    if (arg == "custom") {
        append_line(P_STATUS, "provider set to custom (use /set or amber.conf to configure)");
        return;
    }
    cfg_.apply_provider(arg);
    if (prov->requires_key && cfg_.api_key.empty()) {
        append_line(P_STATUS, "warning: " + arg + " requires an API key (set AMBER_API_KEY)");
    }
    std::string global = agent::global_config_path();
    cfg_.save_global(global);
    append_line(P_STATUS, "provider switched to " + arg + " (saved to " + global + ")");
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
        append_line(P_STATUS, "Keys:  Enter/Ctrl-G send   PgUp/PgDn scroll   Ctrl+P/N history");
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

namespace {
const char* job_state_name(agent::JobState s) {
    switch (s) {
        case agent::JobState::Starting: return "starting";
        case agent::JobState::Running:  return "running";
        case agent::JobState::Done:     return "done";
        case agent::JobState::Killed:   return "killed";
        case agent::JobState::Failed:   return "failed";
    }
    return "?";
}
std::string job_countdown(const agent::JobInfo& i) {
    long rem = i.remaining_hard_s;
    if (i.remaining_idle_s >= 0 &&
        (rem < 0 || i.remaining_idle_s < rem))
        rem = i.remaining_idle_s;
    if (rem < 0) return "";
    return " ~" + std::to_string(rem) + "s";
}
std::string job_list_line(const agent::JobInfo& j) {
    return "id " + j.id + "  " + job_state_name(j.state) + "  pid " +
           std::to_string(j.pid) + "  age " +
           std::to_string(j.seconds_since_start) + "s  idle " +
           std::to_string(j.seconds_since_output) + "s" + job_countdown(j) +
           "  " + j.command;
}
} // namespace

void Tui::cmd_compress(const std::string&) {
    auto& w = win();
    if (!w.agent) {
        append_line(P_STATUS, "no active session to compress");
        return;
    }
    append_line(P_STATUS, "compressing... (see terminal for progress)");
    draw();
    write(STDERR_FILENO, "--- /compress ---\n", 18);
    auto r = w.agent->compress_now();
    write(STDERR_FILENO, "--- /compress done ---\n", 23);
    if (r.messages_before == 0) {
        append_line(P_STATUS, "compress: no compressor configured");
        return;
    }

    // Recalculate context gauge from compressed size
    ctx_used_ = static_cast<long>(r.tokens_after);

    if (r.messages_after >= r.messages_before) {
        append_line(P_STATUS, "compress: nothing to prune ("
                    + std::to_string(r.messages_before)
                    + " messages, ~" + std::to_string(r.tokens_before)
                    + " tokens)");
        return;
    }
    append_line(P_STATUS,
                "compress: " + std::to_string(r.messages_before)
                + " → " + std::to_string(r.messages_after)
                + " msgs, ~" + std::to_string(r.tokens_before)
                + " → ~" + std::to_string(r.tokens_after) + " tokens"
                + "  (core:" + std::to_string(r.core_count)
                + " ctx:" + std::to_string(r.context_count)
                + " prune:" + std::to_string(r.prune_count) + ")");

    auto ext = w.agent->last_extraction_result();
    if (ext.new_memories > 0 || ext.new_skills > 0) {
        append_line(P_STATUS,
                    "  extracted: " + std::to_string(ext.new_memories)
                    + " memories, " + std::to_string(ext.new_skills)
                    + " skills");
    }
    dirty_ = true;
}

void Tui::cmd_job(const std::string& arg) {
    std::string cmd, rest;
    size_t sp = arg.find(' ');
    if (sp == std::string::npos) cmd = arg;
    else { cmd = arg.substr(0, sp); rest = arg.substr(sp + 1); }
    if (cmd.empty() || cmd == "ls") job_ls();
    else if (cmd == "kill") job_kill(rest);
    else if (cmd == "read") job_read(rest);
    else if (cmd == "start") job_start(rest);
    else append_line(P_STATUS,
                     "usage: /job [ls|kill <id>|read <id>|start <cmd>]");
}

void Tui::job_ls() {
    auto jobs = jobs_.list();
    if (jobs.empty()) { append_line(P_STATUS, "no background jobs"); return; }
    for (const auto& j : jobs) append_line(P_STATUS, job_list_line(j));
}

void Tui::job_kill(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /job kill <id>"); return; }
    bool ok = jobs_.stop(id);
    append_line(P_STATUS, ok ? ("killed " + id) : ("no such job: " + id));
    draw();
}

void Tui::job_read(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /job read <id>"); return; }
    std::string out = jobs_.output(id);
    if (out.empty()) { append_line(P_STATUS, "no output for " + id); return; }
    rich::Line body;
    rich::Run run;
    run.text = out;
    run.pair = P_STATUS;
    body.runs.push_back(run);
    for (auto& l : rich::wrap(body, width())) append_rich(l);
}

void Tui::job_start(const std::string& cmd) {
    if (cmd.empty()) { append_line(P_STATUS, "usage: /job start <command>"); return; }
    std::string id = jobs_.start(cmd, agent::Workspace::root());
    if (id.empty()) { append_line(P_STATUS, "failed to start: " + cmd); return; }
    append_line(P_STATUS, "started " + id + ": " + cmd);
    draw();
}

void Tui::config_screen() const {
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
    // Step 1: Select provider via visual list
    std::vector<std::string> prov_choices = {
        "OpenRouter  (openrouter.ai)",
        "Kilo Code   (api.kilocode.ai)",
        "Custom      (user-defined endpoint)"
    };
    int prov_idx = 0;
    if (cfg_.provider_name == "openrouter") prov_idx = 0;
    else if (cfg_.provider_name == "kilocode") prov_idx = 1;
    else prov_idx = 2;

    ModalScope scope;
    curs_set(0);
    {
        ListPanel lp("Select Provider", prov_choices);
        int r = lp.run();
        if (r < 0) return;
        prov_idx = r;
    }

    // Apply provider preset
    std::string new_provider;
    if (prov_idx == 0) new_provider = "openrouter";
    else if (prov_idx == 1) new_provider = "kilocode";
    else new_provider = "custom";

    if (new_provider != "custom") {
        cfg_.apply_provider(new_provider);
        cfg_.provider_name = new_provider;
    }

    // Step 2: Edit provider fields
    std::string model_field = cfg_.model_explicit ? cfg_.model : "";
    std::string ctx_field =
        cfg_.context_explicit ? std::to_string(cfg_.context_size) : "0";
    std::vector<FieldSpec> fields = {
        {"Server URL", cfg_.api_base, false},
        {"API Key", cfg_.api_key, true},
        {"Model (blank = auto)", model_field, false},
        {"Context n_ctx (0 = auto)", ctx_field, false},
    };
    if (!form_edit("Provider Settings", fields)) return;

    cfg_.api_base = fields[0].value;
    while (!cfg_.api_base.empty() && cfg_.api_base.back() == '/')
        cfg_.api_base.pop_back();
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

    // Step 3: Test connection or skip
    std::string det = last_detected_.ok
        ? ("detected: " + last_detected_.model + " / n_ctx " +
           std::to_string(last_detected_.context_size))
        : "detected: (none)";
    int post = menu_select(det, {"Test connection & auto-fill", "Save settings", "Cancel"});
    if (post == 0) {
        test_connection(true);
        // Save after detection fills values
        cfg_.save_global(agent::global_config_path());
        append_line(P_STATUS, "settings saved");
    } else if (post == 1) {
        cfg_.save_global(agent::global_config_path());
        append_line(P_STATUS, "settings saved to " + agent::global_config_path());
    }
}

void Tui::save_settings() { cfg_.save(settings_path_); }

} // namespace tui

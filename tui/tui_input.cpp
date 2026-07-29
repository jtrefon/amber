// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "tui/list_panel.h"
#include "tui/confirm_panel.h"
#include "agent/model_probe.h"

#include <algorithm>
#include <csignal>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace tui {

namespace {

int map_last_choice(agent::PolicyLevel lc) {
    switch (lc) {
        case agent::PolicyLevel::AllowSession: return 1;
        case agent::PolicyLevel::AlwaysAllow:  return 2;
        case agent::PolicyLevel::AlwaysDeny:   return 3;
        default: return 0;
    }
}

} // namespace

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
    hooks.on_approval = [this](const std::string& tool, const agent::json&,
                                const std::string& summary) -> agent::Approval {
        flush_stream();
        int dflt = 0;
        if (auto* ag = win().agent.get())
            dflt = map_last_choice(ag->policy().last_choice(tool));
        agent::Approval d = approve_dialog(summary, 60, dflt);
        const char* verdict = "denied";
        if (d == agent::Approval::AllowOnce) verdict = "allowed once";
        else if (d == agent::Approval::AllowSession) verdict = "allowed session";
        else if (d == agent::Approval::AlwaysAllow) verdict = "always allow";
        else if (d == agent::Approval::AlwaysDeny) verdict = "always deny";
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


void Tui::cmd_set(const std::string& arg) {
    // Try the SettingRegistry first for both dotted AND space-separated keys.
    // Convert "detection loop on" → try "detection.loop", value "on".
    auto try_registry = [&](const std::string& dotted_key, const std::string& val) -> bool {
        const Setting* s = settings_.find(dotted_key);
        if (s && s->setter && !val.empty()) {
            s->setter(std::string(val));
            append_line(P_STATUS, s->key + ": " + s->getter() + "  —  " + s->help);
            cfg_.save_settings(settings_path_);
            return true;
        }
        return false;
    };
    // For space-separated args like "detection loop on", try dotted form "detection.loop".
    {
        size_t last_sp = arg.rfind(' ');
        if (last_sp != std::string::npos) {
            std::string space_key = arg.substr(0, last_sp);
            std::string space_val = arg.substr(last_sp + 1);
            // Convert space-separated key to dotted.
            std::string dotted;
            size_t p = 0;
            while (p < space_key.size()) {
                size_t sp = space_key.find(' ', p);
                if (sp == std::string::npos) { dotted += space_key.substr(p); break; }
                if (!dotted.empty()) dotted += ".";
                dotted += space_key.substr(p, sp - p);
                p = sp + 1;
            }
            if (!dotted.empty() && try_registry(dotted, space_val))
                return;
        }
    }
    // Fast path: dotted keys via SettingRegistry (e.g. "compression.threshold 0.8").
    if (arg.find('.') != std::string::npos) {
        size_t sp = arg.find(' ');
        std::string key = (sp == std::string::npos) ? arg : arg.substr(0, sp);
        std::string val = (sp == std::string::npos) ? "" : arg.substr(sp + 1);
        const Setting* s = settings_.find(key);
        if (s && s->setter && !val.empty()) {
            s->setter(std::string(val));
            append_line(P_STATUS, s->key + ": " + s->getter() + "  —  " + s->help);
            cfg_.save_settings(settings_path_);
            return;
        }
        if (s && s->help.empty() && val.empty()) {
            append_line(P_STATUS, s->help + "  (" + s->placeholder + ")");
            return;
        }
    }
    if (arg.empty()) {
        // BitchX-style: /set alone shows all settings
        append_line(P_STATUS, "detection loop: " + std::string(cfg_.detection_loop ? "on" : "off"));
        append_line(P_STATUS, "detection duplicate: " + std::string(cfg_.detection_duplicate ? "on" : "off"));
        append_line(P_STATUS, "display markdown: " + std::string(win().markdown_on ? "on" : "off"));
        std::string tf = (tool_fold_ == ToolFold::Always) ? "always" :
                         (tool_fold_ == ToolFold::Never) ? "never" : "auto";
        append_line(P_STATUS, "toolfold: " + tf);
        std::string pol = (cfg_.mode == agent::AgentMode::Read) ? "read" :
                          (cfg_.mode == agent::AgentMode::Yolo) ? "yolo" : "write";
        append_line(P_STATUS, "policy: " + pol);
        append_line(P_STATUS, "compression threshold: " +
            std::to_string(cfg_.compression_threshold > 0.0 ? cfg_.compression_threshold : 0.75));
        append_line(P_STATUS, "compression min_turns: " +
            std::to_string(cfg_.compression_min_turns > 0 ? cfg_.compression_min_turns : 10));
        append_line(P_STATUS, "provider: " + cfg_.provider_name);
        append_line(P_STATUS, "model: " + cfg_.model);
        append_line(P_STATUS, "thinking: " + cfg_.thinking);
        append_line(P_STATUS, "Use /set <option> <value> to change a setting");
        draw();
        return;
    }

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

    // policy: mode read|write|yolo, or tool <tool> <level>, or timeout <N>
    if (ns == "policy") {
        // /set policy read|write|yolo — backward compat shortcut for mode
        if (rest == "read" || rest == "write" || rest == "yolo") {
            cfg_.mode = (rest == "read") ? agent::AgentMode::Read :
                        (rest == "yolo") ? agent::AgentMode::Yolo :
                                           agent::AgentMode::Write;
            append_line(P_STATUS, "policy mode: " + rest);
            draw();
            return;
        }
        // /set policy rule <name> <allow|deny|ask> — permission rule
        size_t sp_rule = rest.find(' ');
        std::string subcmd = (sp_rule == std::string::npos) ? rest : rest.substr(0, sp_rule);
        std::string subargs = (sp_rule == std::string::npos) ? "" : rest.substr(sp_rule + 1);
        if (subcmd == "rule") {
            size_t sp2 = subargs.find(' ');
            std::string name = (sp2 == std::string::npos) ? subargs : subargs.substr(0, sp2);
            std::string lvl = (sp2 == std::string::npos) ? "" : subargs.substr(sp2 + 1);
            if (name.empty() || lvl.empty()) {
                append_line(P_STATUS, "usage: /set policy rule <name> <allow|deny|ask>");
                draw();
                return;
            }
            agent::PolicyLevel pl = agent::policy_level_from_name(lvl);
            if (pl == agent::PolicyLevel::Ask) {
                for (auto& w : windows_)
                    if (w->agent) w->agent->policy().revoke(name);
                append_line(P_STATUS, "policy rule revoked for " + name);
            } else if (pl == agent::PolicyLevel::AlwaysAllow || pl == agent::PolicyLevel::AlwaysDeny) {
                for (auto& w : windows_)
                    if (w->agent) w->agent->policy().set_rule(name, pl);
                append_line(P_STATUS, "policy rule " + lvl + " for " + name);
            } else {
                append_line(P_STATUS, "invalid level: " + lvl + " (use allow, deny, or ask)");
                draw();
                return;
            }
            if (win().agent) {
                std::string policy_path = agent::Workspace::local_dir() + "/policy.json";
                win().agent->policy().save(policy_path);
            }
            draw();
            return;
        }
        append_line(P_STATUS, "usage: /set policy mode <read|write|yolo> | /set policy rule <name> <allow|deny|ask> | /set policy timeout <N> | /set policy approval <on|off>");
        draw();
        return;
        if (win().agent) {
            std::string policy_path = agent::Workspace::local_dir() + "/policy.json";
            win().agent->policy().save(policy_path);
        }
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

    // compression threshold <0.0-1.0> | min_turns <N>
    if (ns == "compression") {
        size_t sp2 = rest.find(' ');
        std::string key = (sp2 == std::string::npos) ? "" : rest.substr(0, sp2);
        std::string val = (sp2 == std::string::npos) ? "" : rest.substr(sp2 + 1);
        if (key == "threshold") {
            double t = std::atof(val.c_str());
            if (t <= 0.0 || t > 1.0 || val.empty()) {
                append_line(P_STATUS, "usage: /set compression threshold <0.1-1.0>");
                return;
            }
            cfg_.compression_threshold = t;
            for (auto& w : windows_)
                if (w->agent) w->agent->set_compression_threshold(t);
            append_line(P_STATUS, "compression threshold: " + std::to_string(t));
        } else if (key == "min_turns") {
            int n = std::atoi(val.c_str());
            if (n < 1 || val.empty()) {
                append_line(P_STATUS, "usage: /set compression min_turns <1-999>");
                return;
            }
            cfg_.compression_min_turns = n;
            for (auto& w : windows_)
                if (w->agent) w->agent->set_compression_min_turns(n);
            append_line(P_STATUS, "compression min_turns: " + std::to_string(n));
        } else {
            append_line(P_STATUS, "usage: /set compression threshold|min_turns <value>");
        }
        if (!cfg_.save_settings(settings_path_))
            append_line(P_STATUS, "warning: could not save to " + settings_path_);
        draw();
        return;
    }

    append_line(P_STATUS, "unknown option: " + ns + " (try: detection, display, toolfold, policy, compression, provider, model, think)");
}

void Tui::cmd_get(const std::string& arg) {
    // Try the SettingRegistry first — it handles dotted keys (detection.loop)
    // AND any key registered in build_settings() (model, provider, toolfold, etc.).
    const Setting* reg = settings_.find(arg);
    if (reg && reg->getter) {
        std::string help_text = settings_.help_for(arg);
        std::string msg = arg + ": " + reg->getter();
        if (!help_text.empty()) msg += "  —  " + help_text;
        append_line(P_STATUS, msg);
        return;
    }
    // Also try appending suffixes for namespace-level queries (e.g. "detection" → "detection.loop").
    auto subs = settings_.keys_in(arg);
    if (!subs.empty()) {
        for (const auto& sub : subs)
            cmd_get(arg + "." + sub);
        return;
    }

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
    if (arg.rfind("policy", 0) == 0) {
        std::string sub = (arg.size() > 7) ? arg.substr(7) : "";
        if (sub.empty() || sub == " ") {
            // Show mode
            std::string v = (cfg_.mode == agent::AgentMode::Read) ? "read" :
                            (cfg_.mode == agent::AgentMode::Yolo) ? "yolo" : "write";
            append_line(P_STATUS, "policy mode: " + v);
            append_line(P_STATUS, "policy timeout: " + std::to_string(policy_timeout_) + "s");
            // List stored rules
            auto* ag = win().agent.get();
            if (ag) {
                for (const auto& r : ag->policy().rules()) {
                    if (r.level == agent::PolicyLevel::Ask) continue;
                    append_line(P_STATUS, "  " + r.tool + " → " +
                        agent::policy_level_name(r.level) +
                        " (used " + std::to_string(r.count) + "x)");
                }
            }
            return;
        }
        // /get policy rule <name> — show specific rule
        std::string name = sub;
        if (!name.empty() && name[0] == ' ') name = name.substr(1);
        if (name.rfind("rule ", 0) == 0) name = name.substr(5);
        if (!name.empty()) {
            if (auto* ag = win().agent.get()) {
                const auto* r = ag->policy().find(name);
                if (r) {
                    append_line(P_STATUS, "rule " + r->tool + ": " +
                        agent::policy_level_name(r->level) +
                        " (last: " + agent::policy_level_name(r->last_choice) +
                        ", used " + std::to_string(r->count) + "x)");
                } else {
                    append_line(P_STATUS, "rule " + name + ": ask (no stored rule)");
                }
            }
            return;
        }
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
    if (arg.rfind("compression", 0) == 0) {
        double t = (cfg_.compression_threshold > 0.0) ? cfg_.compression_threshold : 0.75;
        int mt = (cfg_.compression_min_turns > 0) ? cfg_.compression_min_turns : 10;
        append_line(P_STATUS, "compression threshold: " + std::to_string(t));
        append_line(P_STATUS, "compression min_turns: " + std::to_string(mt));
        return;
    }
    // Look up dotted keys via the SettingRegistry (e.g. "compression.threshold").
    const Setting* s = settings_.find(arg);
    if (s && s->getter) {
        append_line(P_STATUS, s->key + ": " + s->getter() + "  —  " + s->help);
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
        {"new", {"clear", "reset"}, "",
         "clear the current conversation and start fresh",
         [this](const std::string&) {
             if (win().agent) {
                 win().agent->set_context({});
                 win().agent->policy().clear_session();
             }
             win().stream_buf.clear();
             win().stream_ts.clear();
             win().reason_buf.clear();
             win().reason_folded = false;
             win().scroll_top = 0;
             ctx_used_ = -1;
             live_ctx_offset_ = 0;
             append_line(P_STATUS, "conversation cleared — next message starts fresh");
             drawer_open_ = false;
             draw();
         }},
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
         "set runtime options: detection, display, toolfold, policy (mode|tool <name> <level>|timeout <N>), compression, provider, model, think",
          [this](const std::string& a) { cmd_set(a); },
          nullptr,
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
         "show current setting: config, model, provider, toolfold, policy (mode|timeout|<tool>), display, compression, detection",
          [this](const std::string& a) { cmd_get(a); }},
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

        // Provider management (CRUD)
        {"provider", {"p"}, "list|add|edit|delete|test",
         "manage API providers",
         [this](const std::string& a) {
             if (a.empty()) { settings_screen(); redraw_after_modal(); return; }
             std::string sub, rest; size_t sp = a.find(' ');
             if (sp == std::string::npos) { sub = a; rest.clear(); }
             else { sub = a.substr(0, sp); rest = a.substr(sp + 1); }
             if (sub == "list") {
                 auto providers = agent::list_saved_providers();
                 std::string msg;
                 for (auto& p : providers) msg += p + " ";
                 append_line(P_STATUS, "providers: " + (msg.empty() ? "(none)" : msg));
             } else if (sub == "add" || sub == "edit") {
                 append_line(P_STATUS, "use /settings to add/edit providers");
             } else if (sub == "delete") {
                 agent::delete_provider(rest);
                 append_line(P_STATUS, "deleted provider: " + rest);
             } else if (sub == "test") {
                 append_line(P_STATUS, "testing " + rest + "...");
                 agent::Config test_cfg = cfg_;
                 agent::load_provider(rest, test_cfg);
                 agent::ServerInfo info = agent::probe_server(test_cfg);
                 append_line(P_STATUS, rest + ": " + (info.ok ? "OK" : "FAILED"));
             } else {
                 // Set active provider by name.
                    cfg_.apply_provider(a);
                    cfg_.save_global(agent::global_config_path());
                 append_line(P_STATUS, "provider: " + a);
             }
         }},

        // Model management (CRUD)
        {"model", {"m"}, "list|set|probe",
         "manage AI models",
         [this](const std::string& a) {
             std::string sub, rest; size_t sp = a.find(' ');
             if (sp == std::string::npos) { sub = a; rest.clear(); }
             else { sub = a.substr(0, sp); rest = a.substr(sp + 1); }
             if (sub == "list" || sub.empty()) {
                 auto models = agent::list_models(cfg_);
                 std::string msg;
                 for (auto& m : models) msg += m + " ";
                 if (msg.size() > 200) msg.resize(200);
                 append_line(P_STATUS, "models: " + (msg.empty() ? "(none)" : msg));
             } else if (sub == "probe") {
                 auto info = agent::probe_server(cfg_);
                 if (info.ok)
                     append_line(P_STATUS, "model: " + info.model + " ctx: " + std::to_string(info.context_size));
                 else
                     append_line(P_STATUS, "probe failed");
             } else {
                    cfg_.model = a; cfg_.model_explicit = true;
                cfg_.save_global(agent::global_config_path());
                 append_line(P_STATUS, "model: " + a);
             }
         }},

        // Session management (expanded CRUD)
        {"session", {}, "list|save|load|delete|rename <id> <title>",
         "manage saved sessions",
         [this](const std::string& a) {
             std::string sub, rest; size_t sp = a.find(' ');
             if (sp == std::string::npos) { sub = a; rest.clear(); }
             else { sub = a.substr(0, sp); rest = a.substr(sp + 1); }
             if (sub == "list" || sub.empty()) {
                 session_browser();
             } else if (sub == "save") {
                 save_session();
             } else if (sub == "load") {
                 if (!rest.empty()) {
                     agent::Session s;
                     store_.load(rest, s);
                     if (s.id.empty())
                         append_line(P_STATUS, "session not found: " + rest);
                     else
                         load_session(rest);
                 } else {
                     session_browser();
                 }
             } else if (sub == "delete") {
                 if (!rest.empty()) {
                     store_.remove(rest);
                     append_line(P_STATUS, "deleted session: " + rest);
                 }
             } else if (sub == "rename") {
                 append_line(P_STATUS, "rename not yet implemented");
             }
         }},

        // Job aliases and expanded management
        // (job already exists above — aliases are in the alias list)

        // File browsing
        {"files", {"f"}, "ls|tree|open|find <path>",
         "browse and view files in the workspace",
         [this](const std::string& a) {
             std::string sub, rest; size_t sp = a.find(' ');
             if (sp == std::string::npos) { sub = a; rest.clear(); }
             else { sub = a.substr(0, sp); rest = a.substr(sp + 1); }
             namespace fs = std::filesystem;
             std::string root = agent::Workspace::root();
             if (rest.empty()) rest = ".";
             if (rest[0] != '/') rest = root + "/" + rest;
             fs::path p(rest);
             if (sub == "ls" || sub.empty()) {
                 if (!fs::exists(p)) { append_line(P_STATUS, "not found: " + rest); return; }
                 if (fs::is_directory(p)) {
                     std::string out;
                     for (const auto& e : fs::directory_iterator(p))
                         out += e.path().filename().string() + "  ";
                     if (out.empty()) out = "(empty)";
                     append_line(P_ASSISTANT, out);
                 } else {
                     append_line(P_ASSISTANT, p.filename().string());
                 }
             } else if (sub == "tree") {
                 std::string out;
                 if (fs::exists(p) && fs::is_directory(p)) {
                     std::function<void(const fs::path&, int)> walk;
                     walk = [&](const fs::path& dir, int depth) {
                         if (depth > 3) return;
                         for (const auto& e : fs::directory_iterator(dir)) {
                             for (int i = 0; i < depth; ++i) out += "  ";
                             out += e.path().filename().string() + "\n";
                             if (fs::is_directory(e)) walk(e.path(), depth + 1);
                         }
                     };
                     walk(p, 0);
                 }
                 if (out.empty()) out = "(empty)";
                 append_line(P_ASSISTANT, out);
             } else if (sub == "open") {
                 if (!fs::exists(p) || fs::is_directory(p)) {
                     append_line(P_STATUS, "not a file: " + rest);
                 } else {
                     std::ifstream f(p);
                     std::string content((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
                     if (content.size() > 4096) content.resize(4096);
                     append_line(P_ASSISTANT, p.filename().string() + ":\n" + content);
                 }
             } else if (sub == "find") {
                 if (!fs::exists(p) || !fs::is_directory(p)) {
                     append_line(P_STATUS, "not a directory: " + rest);
                 } else {
                     // rest is the search root; we need a pattern
                     size_t sp2 = a.find(' ', sp + 1);
                    std::string pattern;
                    if (sp2 == std::string::npos) {
                         // no pattern given — show directory contents
                         for (const auto& e : fs::directory_iterator(p))
                             append_line(P_ASSISTANT, e.path().filename().string());
                     }
                 }
             }
         }},

        // System operations
        {"system", {"sy"}, "exec|delete|rmdir|mkdir|mv|cp|info|ps|kill|df|uptime|uname",
         "system operations (file mgmt, processes, disk)",
         [this](const std::string& a) {
             std::string sub, rest; size_t sp = a.find(' ');
             if (sp == std::string::npos) { sub = a; rest.clear(); }
             else { sub = a.substr(0, sp); rest = a.substr(sp + 1); }

             auto run_cmd = [&](const std::string& cmd) -> std::string {
                 FILE* f = popen(cmd.c_str(), "r");
                 if (!f) return "(popen failed)";
                 std::string out;
                 char buf[4096];
                 while (fgets(buf, sizeof(buf), f)) out += buf;
                 pclose(f);
                 return out;
             };

             namespace fs = std::filesystem;

             if (sub == "exec") {
                 if (rest.empty()) { append_line(P_STATUS, "usage: /system exec <command>"); return; }
                 std::string out = run_cmd(rest);
                 if (!out.empty() && out.back() == '\n') out.pop_back();
                 append_line(P_ASSISTANT, out.empty() ? "(no output)" : out);
             } else if (sub == "delete") {
                 if (rest.empty()) { append_line(P_STATUS, "usage: /system delete <path>"); return; }
                 std::error_code ec;
                 if (fs::remove(fs::path(rest), ec))
                     append_line(P_STATUS, "deleted: " + rest);
                 else
                     append_line(P_STATUS, "delete failed: " + ec.message());
             } else if (sub == "rmdir") {
                 if (rest.empty()) { append_line(P_STATUS, "usage: /system rmdir <path>"); return; }
                 std::error_code ec;
                 if (fs::remove_all(fs::path(rest), ec) > 0)
                     append_line(P_STATUS, "removed: " + rest);
                 else
                     append_line(P_STATUS, "remove failed: " + ec.message());
             } else if (sub == "mkdir") {
                 if (rest.empty()) { append_line(P_STATUS, "usage: /system mkdir <path>"); return; }
                 std::error_code ec;
                 if (fs::create_directories(fs::path(rest), ec))
                     append_line(P_STATUS, "created: " + rest);
                 else
                     append_line(P_STATUS, "mkdir failed: " + ec.message());
             } else if (sub == "mv") {
                 size_t sp2 = rest.find(' ');
                 if (sp2 == std::string::npos) { append_line(P_STATUS, "usage: /system mv <src> <dst>"); return; }
                 std::string src = rest.substr(0, sp2), dst = rest.substr(sp2 + 1);
                 std::error_code ec;
                 fs::rename(fs::path(src), fs::path(dst), ec);
                 if (!ec)
                     append_line(P_STATUS, "moved: " + src + " -> " + dst);
                 else
                     append_line(P_STATUS, "mv failed: " + ec.message());
             } else if (sub == "cp") {
                 size_t sp2 = rest.find(' ');
                 if (sp2 == std::string::npos) { append_line(P_STATUS, "usage: /system cp <src> <dst>"); return; }
                 std::string src = rest.substr(0, sp2), dst = rest.substr(sp2 + 1);
                 std::error_code ec;
                 fs::copy(fs::path(src), fs::path(dst), fs::copy_options::recursive, ec);
                 if (!ec)
                     append_line(P_STATUS, "copied: " + src + " -> " + dst);
                 else
                     append_line(P_STATUS, "cp failed: " + ec.message());
             } else if (sub == "info") {
                 if (rest.empty()) { append_line(P_STATUS, "usage: /system info <path>"); return; }
                 std::error_code ec;
                 auto s = fs::status(fs::path(rest), ec);
                 if (ec) { append_line(P_STATUS, "stat failed: " + ec.message()); return; }
                 std::string type;
                 if (fs::is_directory(s)) type = "directory";
                 else if (fs::is_regular_file(s)) type = "file";
                 else if (fs::is_symlink(s)) type = "symlink";
                 else type = "other";
                 auto size = fs::file_size(fs::path(rest), ec);
                 append_line(P_STATUS, rest + ": " + type + " " + std::to_string(size) + " bytes");
             } else if (sub == "ps") {
                 std::string out = run_cmd("ps aux --sort=-%mem 2>/dev/null | head -20 || ps aux 2>/dev/null | head -20");
                 if (!out.empty() && out.back() == '\n') out.pop_back();
                 append_line(P_ASSISTANT, out.empty() ? "(no output)" : out);
             } else if (sub == "kill") {
                 if (rest.empty()) { append_line(P_STATUS, "usage: /system kill <pid>"); return; }
                 int pid = std::atoi(rest.c_str());
                 if (pid > 0 && ::kill(pid, SIGTERM) == 0)
                     append_line(P_STATUS, "killed: " + rest);
                 else
                     append_line(P_STATUS, "kill failed");
             } else if (sub == "df") {
                 std::string out = run_cmd("df -h " + rest + " 2>/dev/null || df -h . 2>/dev/null");
                 if (!out.empty() && out.back() == '\n') out.pop_back();
                 append_line(P_ASSISTANT, out.empty() ? "(no output)" : out);
             } else if (sub == "uptime") {
                 std::string out = run_cmd("uptime 2>/dev/null");
                 if (!out.empty() && out.back() == '\n') out.pop_back();
                 append_line(P_ASSISTANT, out.empty() ? "(no output)" : out);
             } else if (sub == "uname") {
                 std::string out = run_cmd("uname -a 2>/dev/null");
                 if (!out.empty() && out.back() == '\n') out.pop_back();
                 append_line(P_ASSISTANT, out.empty() ? "(no output)" : out);
             } else {
                 append_line(P_STATUS, "unknown system command: " + sub);
             }
         }},
    };
}

const Command* Tui::find_command(const std::string& name) {
    return palette::find(commands(), name);
}

bool Tui::handle_slash(const std::string& line) {
    if (line.empty() || line[0] != '/') return false;
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();
    std::string rest = trimmed.substr(1);
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
        // Spawn a ListPanel with all available models (with filter/search)
        append_line(P_STATUS, "querying " + cfg_.models_url() + " ...");
        draw();
        auto models = agent::list_models(cfg_);
        if (models.empty()) {
            append_line(P_STATUS, "no models available or server unreachable");
            return;
        }

        // Mark current model
        std::vector<std::string> display;
        for (size_t i = 0; i < models.size(); ++i) {
            bool cur = (models[i] == cfg_.model);
            display.push_back((cur ? "> " : "  ") + models[i]);
        }

        {
            ModalScope scope;
            curs_set(0);
            ListPanel lp("Select Model (" + std::to_string(models.size()) + " available)",
                         display);
            int sel = lp.run();
            if (sel < 0) return;
            // Map back to original model name (the UI string has "> " or "  " prefix)
            std::string chosen = display[sel].substr(2);
            cfg_.model = chosen;
            cfg_.model_explicit = true;
        }

        for (auto& w : windows_) {
            if (!w->agent) continue;
            w->agent->set_detection_loop(cfg_.detection_loop);
        }
        std::string global = agent::global_config_path();
        cfg_.save_global(global);
        append_line(P_STATUS, "model set to " + cfg_.model + " (saved to " + global + ")");
        return;
    }

    // Set model by name (direct)
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
    for (auto& w : windows_) {
        if (!w->agent) continue;
        w->agent->set_detection_loop(cfg_.detection_loop);
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
    if (!prov || (prov->name == "custom" && arg != "custom")) {
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
    if (agent_busy_.load()) {
        append_line(P_STATUS, "compress: agent is busy");
        return;
    }
    append_line(P_STATUS, "compressing...");
    state_ = agent::RunState::Waiting;
    std::thread t([this] { compress_worker(); });
    t.detach();
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

static bool edit_provider_form(agent::Config& cfg, const std::string& title) {
    std::string model_field = cfg.model_explicit ? cfg.model : "";
    std::string ctx_field =
        cfg.context_explicit ? std::to_string(cfg.context_size) : "0";
    std::vector<FieldSpec> fields = {
        {"Server URL", cfg.api_base, false},
        {"API Key", cfg.api_key, true},
        {"Model (blank = auto)", model_field, false},
        {"Context n_ctx (0 = auto)", ctx_field, false},
    };
    if (!form_edit(title, fields)) return false;
    cfg.api_base = fields[0].value;
    while (!cfg.api_base.empty() && cfg.api_base.back() == '/')
        cfg.api_base.pop_back();
    cfg.api_key = fields[1].value;
    if (fields[2].value.empty()) {
        cfg.model_explicit = false;
    } else {
        cfg.model = fields[2].value;
        cfg.model_explicit = true;
    }
    try {
        int n = std::stoi(fields[3].value);
        if (n > 0) { cfg.context_size = n; cfg.context_explicit = true; }
        else       { cfg.context_explicit = false; }
    } catch (...) { cfg.context_explicit = false; }
    return true;
}

void Tui::settings_screen() {
    // Step 1: Build provider list from saved + built-in presets
    // DEBUG: the fact you can see this message means the NEW settings_screen is running
    append_line(P_STATUS, "Loading provider list...");
    auto saved = agent::list_saved_providers();

    std::vector<std::string> prov_display;
    std::vector<std::string> prov_id;
    int active_idx = -1;

    // Built-in presets
    auto add_preset = [&](const std::string& id, const std::string& label) {
        prov_display.push_back((cfg_.provider_name == id ? "> " : "  ") + label);
        prov_id.push_back(id);
        if (cfg_.provider_name == id) active_idx = static_cast<int>(prov_id.size() - 1);
    };
    add_preset("openrouter", "OpenRouter  (openrouter.ai)");
    add_preset("kilocode",   "Kilo Code   (api.kilocode.ai)");
    add_preset("custom",     "Custom      (user-defined)");

    // User-saved providers
    for (const auto& s : saved) {
        if (s == "openrouter" || s == "kilocode" || s == "custom") continue;
        prov_display.push_back((cfg_.provider_name == s ? "> " : "  ") + s);
        prov_id.push_back(s);
        if (cfg_.provider_name == s) active_idx = static_cast<int>(prov_id.size() - 1);
    }

    // Add "Add new..." option at the end
    int add_new_idx = static_cast<int>(prov_id.size());
    prov_display.push_back("  + Add new provider...");
    prov_id.push_back("");  // sentinel

    if (active_idx < 0) active_idx = 2;  // default to custom

    // Step 2: Select provider or action
    ModalScope scope;
    curs_set(0);
    int sel;
    {
        // Show provider list with summary info
        std::vector<std::string> rich_display;
        for (size_t i = 0; i < prov_id.size(); ++i) {
            std::string id = prov_id[i];
            if (id.empty()) {
                rich_display.push_back(prov_display[i]);
                continue;
            }
            bool active = (id == cfg_.provider_name);
            std::string prefix = active ? "> " : "  ";
            std::string key_hint = cfg_.api_key.empty() ? "no-key" : "key-set";
            if (id == "openrouter" || id == "kilocode" || id == "custom") {
                rich_display.push_back(prefix + id + "  (" + key_hint + ")");
            } else {
                std::string key = cfg_.api_key.empty() && active ? "no-key" : "key-set";
                rich_display.push_back(prefix + id + "  (" + key + ")");
            }
        }
        rich_display.back() = "  + Add new provider...";

        ListPanel lp("Providers (" + std::to_string(prov_id.size() - 1) + " configured)",
                     rich_display);
        sel = lp.run();
    }
    if (sel < 0) return;

    // Handle "Add new provider..."
    if (sel == add_new_idx) {
        // Ask for provider name
        std::vector<FieldSpec> name_field = {{"Provider name", "", false}};
        if (!form_edit("New Provider", name_field)) return;
        std::string new_name = name_field[0].value;
        if (new_name.empty()) return;

        // Check if preset exists
        agent::Config prov_cfg;
        bool is_preset = false;
        for (auto* p : agent::provider::all) {
            if (p->name == new_name) {
                prov_cfg.provider_name = p->name;
                prov_cfg.api_base = p->api_base;
                prov_cfg.model = p->default_model;
                is_preset = true;
                break;
            }
        }
        if (!is_preset) {
            // Try to load saved provider
            if (!agent::load_provider(new_name, prov_cfg)) {
                prov_cfg.provider_name = new_name;
            }
        }
        if (!edit_provider_form(prov_cfg, "Edit: " + new_name)) return;
        prov_cfg.provider_name = new_name;
        agent::save_provider(prov_cfg);
        cfg_.provider_name = new_name;
        cfg_.api_base = prov_cfg.api_base;
        cfg_.api_key = prov_cfg.api_key;
        cfg_.model = prov_cfg.model;
        cfg_.model_explicit = !prov_cfg.model.empty();
        cfg_.save_global(agent::global_config_path());
        append_line(P_STATUS, "provider '" + new_name + "' added and activated");
        return;
    }

    // Handle built-in / saved provider selection
    std::string selected_id = prov_id[sel];
    if (selected_id.empty()) return;

    // Step 3: Show actions for selected provider
    bool is_preset = (selected_id == "openrouter" || selected_id == "kilocode" || selected_id == "custom");
    std::vector<std::string> actions = {"Activate & edit", "Test connection"};
    if (!is_preset) {
        actions.push_back("Delete provider");
    }
    int action = menu_select("Provider: " + selected_id, actions);
    if (action < 0) return;

    if (action == 0) {
        // Activate & edit — start from preset defaults, overlay current values
        agent::Config prov_cfg;
        if (is_preset) {
            prov_cfg.provider_name = selected_id;
            prov_cfg.api_base = cfg_.api_base;
            prov_cfg.api_key = cfg_.api_key;
            prov_cfg.model = cfg_.model;
            prov_cfg.model_explicit = cfg_.model_explicit;
            // If not already using this preset, seed with its defaults
            if (cfg_.provider_name != selected_id) {
                prov_cfg.apply_provider(selected_id);
                if (prov_cfg.api_key.empty()) prov_cfg.api_key = cfg_.api_key;
            }
        } else {
            agent::load_provider(selected_id, prov_cfg);
        }
        if (!edit_provider_form(prov_cfg, "Edit: " + selected_id)) return;

        if (is_preset) {
            cfg_.provider_name = selected_id;
            if (selected_id != "custom") {
                cfg_.apply_provider(selected_id);
            }
            cfg_.api_base = prov_cfg.api_base;
            cfg_.api_key = prov_cfg.api_key;
            if (!prov_cfg.model.empty()) {
                cfg_.model = prov_cfg.model;
                cfg_.model_explicit = true;
            }
        } else {
            prov_cfg.provider_name = selected_id;
            agent::save_provider(prov_cfg);
            cfg_.provider_name = selected_id;
            cfg_.api_base = prov_cfg.api_base;
            cfg_.api_key = prov_cfg.api_key;
            cfg_.model = prov_cfg.model;
            cfg_.model_explicit = !prov_cfg.model.empty();
        }
        cfg_.save_global(agent::global_config_path());
        append_line(P_STATUS, "provider '" + selected_id + "' activated");

        // Test connection
        test_connection(false);
    }
    else if (action == 1) {
        // Test connection
        cfg_.provider_name = selected_id;
        cfg_.save_global(agent::global_config_path());
        test_connection(true);
    }
    else if (action == 2 && !is_preset) {
        // Delete provider
        std::string msg = "Delete provider \"" + selected_id + "\"?";
        tui::ConfirmPanel confirm("Delete Provider", msg);
        if (confirm.run()) {
            agent::delete_provider(selected_id);
            append_line(P_STATUS, "provider '" + selected_id + "' deleted");
        }
    }
}

void Tui::build_settings() {
    settings_ = tui::SettingRegistry{};
    auto add = [&](const std::string& key, const std::string& help,
                   const std::string& placeholder, Setting::Type type,
                   std::vector<std::string> choices,
                   double rmin, double rmax,
                   std::function<std::string()> getter,
                   std::function<void(const std::string&)> setter) {
        settings_.add({key, help, placeholder, type, choices, rmin, rmax, getter, setter});
    };
    add("detection.loop", "Tool-loop detection", "<on|off|toggle>", Setting::Choice,
        {"on","off","toggle"}, 0, 0,
        [this](){ return cfg_.detection_loop ? "on" : "off"; },
        [this](const std::string& v) {
            if (v == "toggle") cfg_.detection_loop = !cfg_.detection_loop;
            else cfg_.detection_loop = (v == "on");
            cfg_.save_settings(settings_path_);
            for (auto& w : windows_) if (w && w->agent) w->agent->set_detection_loop(cfg_.detection_loop);
        });
    add("detection.duplicate", "Duplicate call detection", "<on|off|toggle>", Setting::Choice,
        {"on","off","toggle"}, 0, 0,
        [this](){ return cfg_.detection_duplicate ? "on" : "off"; },
        [this](const std::string& v) {
            if (v == "toggle") cfg_.detection_duplicate = !cfg_.detection_duplicate;
            else cfg_.detection_duplicate = (v == "on");
            cfg_.save_settings(settings_path_);
        });
    add("display.markdown", "Markdown rendering", "<on|off>", Setting::Choice,
        {"on","off"}, 0, 0,
        [this](){ return win().markdown_on ? "on" : "off"; },
        [this](const std::string& v) {
            win().markdown_on = (v == "on");
            cfg_.save_settings(settings_path_);
        });
    add("toolfold", "Tool result folding mode", "<always|auto|never>", Setting::Choice,
        {"always","auto","never"}, 0, 0,
        [this]() -> std::string {
            return tool_fold_ == ToolFold::Always ? "always" :
                   tool_fold_ == ToolFold::Never ? "never" : "auto";
        },
        [this](const std::string& v) {
            if (v == "always") tool_fold_ = ToolFold::Always;
            else if (v == "never") tool_fold_ = ToolFold::Never;
            else tool_fold_ = ToolFold::Auto;
            cfg_.save_settings(settings_path_);
        });
    add("policy.mode", "Agent mode", "<read|write|yolo>", Setting::Choice,
        {"read","write","yolo"}, 0, 0,
        [this]() -> std::string {
            return cfg_.mode == agent::AgentMode::Read ? "read" :
                   cfg_.mode == agent::AgentMode::Yolo ? "yolo" : "write";
        },
        [this](const std::string& v) {
            if (v == "read") cfg_.mode = agent::AgentMode::Read;
            else if (v == "yolo") cfg_.mode = agent::AgentMode::Yolo;
            else cfg_.mode = agent::AgentMode::Write;
        });
    add("policy.timeout", "Approval dialog timeout", "<0-999>", Setting::Int, {}, 0, 999,
        [this]() -> std::string { return std::to_string(policy_timeout_); },
        [this](const std::string& v) {
            policy_timeout_ = std::stoi(v);
            cfg_.save_settings(settings_path_);
        });
    add("policy.approval", "Enable permission gating in Write mode", "<on|off|toggle>", Setting::Choice,
        {"on","off","toggle"}, 0, 0,
        [this]() -> std::string { return cfg_.policy_approval ? "on" : "off"; },
        [this](const std::string& v) {
            if (v == "toggle") cfg_.policy_approval = !cfg_.policy_approval;
            else cfg_.policy_approval = (v == "on");
            cfg_.save_settings(settings_path_);
            append_line(P_STATUS, std::string("policy approval: ") + (cfg_.policy_approval ? "on" : "off"));
        });
    // Namespace root for /get policy (no setter — children handle values).
    add("policy", "Permission rules and approval settings", "", Setting::String, {}, 0, 0,
        []() -> std::string { return ""; }, nullptr);
    // Simple display-only keys (read-only, for /get completion).
    add("config", "Open the settings configuration screen", "", Setting::String, {}, 0, 0,
        []() -> std::string { return ""; }, nullptr);
    add("model", "Active model name", "", Setting::String, {}, 0, 0,
        [this]() -> std::string { return cfg_.model + " (" + cfg_.provider_name + ")"; }, nullptr);
    add("provider", "Active provider name and API base URL", "", Setting::String, {}, 0, 0,
        [this]() -> std::string { return cfg_.provider_name + " (" + cfg_.api_base + ")"; }, nullptr);

    add("think", "Thinking mode", "<on|off|auto>", Setting::Choice,
        {"on","off","auto"}, 0, 0,
        [this](){ return cfg_.thinking; },
        [this](const std::string& v) { cfg_.thinking = v; cfg_.save_settings(settings_path_); });
    add("compression.threshold", "Context utilisation threshold",
        "<0.1-1.0>", Setting::Float, {}, 0.1, 1.0,
        [this]() -> std::string { return std::to_string(cfg_.compression_threshold > 0 ? cfg_.compression_threshold : 0.75); },
        [this](const std::string& v) {
            cfg_.compression_threshold = std::stod(v);
            cfg_.save_settings(settings_path_);
            for (auto& w : windows_) if (w && w->agent) w->agent->set_compression_threshold(cfg_.compression_threshold);
        });
    add("compression.min_turns", "Minimum turns before compression",
        "<1-999>", Setting::Int, {}, 1, 999,
        [this]() -> std::string { return std::to_string(cfg_.compression_min_turns > 0 ? cfg_.compression_min_turns : 10); },
        [this](const std::string& v) {
            cfg_.compression_min_turns = std::stoi(v);
            cfg_.save_settings(settings_path_);
            for (auto& w : windows_) if (w && w->agent) w->agent->set_compression_min_turns(cfg_.compression_min_turns);
        });
}

} // namespace tui


#include "tui.h"
#include "tui/list_panel.h"
#include "tui/confirm_panel.h"
#include "agent/model_probe.h"
#include "agent/skill_commands.h"
#include "agent/skill_install.h"
#include "agent/mcp_commands.h"

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


namespace {

std::string toolfold_name(ToolFold f) {
    if (f == ToolFold::Always) return "always";
    if (f == ToolFold::Never) return "never";
    return "auto";
}

std::string mode_name(agent::AgentMode m) {
    if (m == agent::AgentMode::Read) return "read";
    if (m == agent::AgentMode::Yolo) return "yolo";
    return "write";
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
        live_ctx_offset_ += (static_cast<long>(d.size()) / 4) + 1;
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


void Tui::cmd_set_detection_toggle(const std::string& key, const std::string& val) {
    if (val != "off" && val != "on" && val != "toggle") {
        append_line(P_STATUS, "usage: /set detection " + key + " off|on|toggle (got: " + val + ")");
        return;
    }
    bool* field = (key == "loop") ? &cfg_.detection_loop : &cfg_.detection_duplicate;
    bool new_val;
    if (val == "on") new_val = true;
    else if (val == "off") new_val = false;
    else new_val = !*field;
    *field = new_val;
    for (auto& w : windows_) {
        if (!w->agent) continue;
        if (key == "loop") w->agent->set_detection_loop(new_val);
        else w->agent->set_detection_duplicate(new_val);
    }
    std::string hint;
    if (key == "loop") hint = new_val ? "breaks on repeat" : "runs until stop";
    else hint = new_val ? "rejects duplicates" : "may repeat calls";
    append_line(P_STATUS, "detection " + key + ": " + (new_val ? "on" : "off") + " \u2014 " + hint);
    if (!cfg_.save_settings(settings_path_))
        append_line(P_STATUS, "warning: could not save to " + settings_path_);
    draw();
}

void Tui::cmd_set_subagent_parallel(const std::string& val) {
    if (val != "off" && val != "on" && val != "toggle") {
        append_line(P_STATUS,
                    "usage: /set subagent parallel on|off|toggle (got: " +
                        val + ")");
        return;
    }
    bool new_val;
    if (val == "on") new_val = true;
    else if (val == "off") new_val = false;
    else new_val = !subagents_.parallel();
    subagents_.set_parallel(new_val);
    cfg_.subagent_parallel = new_val;
    cfg_.save_settings(settings_path_);
    append_line(P_STATUS, std::string("subagent parallel: ") +
                              (new_val ? "on" : "off") +
                              " \u2014 " +
                              (new_val ? "concurrent workers"
                                       : "serial (cache-friendly)"));
    draw();
}

void Tui::cmd_set_subagent_max(const std::string& val) {
    int n = 0;
    try {
        n = std::stoi(val);
    } catch (...) {
        n = -1;
    }
    if (n < 1 || n > 16) {
        append_line(P_STATUS,
                    "usage: /set subagent max <1-16> (got: " + val + ")");
        return;
    }
    subagents_.set_max(n);
    cfg_.subagent_max = n;
    cfg_.save_settings(settings_path_);
    append_line(P_STATUS, "subagent max: " + std::to_string(n));
    draw();
}

void Tui::cmd_get_subagent() {
    append_line(P_STATUS,
                std::string("subagent parallel: ") +
                    (subagents_.parallel() ? "on" : "off") +
                    ", max: " + std::to_string(subagents_.max()));
    draw();
}

void Tui::cmd_set_reasoning_effort(const std::string& val) {
    if (val != "off" && val != "low" && val != "medium" && val != "high") {
        append_line(P_STATUS,
                    "usage: /set reasoning effort off|low|medium|high (got: " +
                        val + ")");
        return;
    }
    cfg_.reasoning_effort = val;
    for (auto& w : windows_)
        if (w->agent) w->agent->set_reasoning_effort(val);
    cfg_.save_settings(settings_path_);
    append_line(P_STATUS, "reasoning effort: " + val +
                              " (applies from the next turn)");
    draw();
}

void Tui::cmd_get_reasoning() {
    append_line(P_STATUS, "reasoning effort: " + cfg_.reasoning_effort);
    draw();
}

void Tui::cmd_set(const std::string& arg) {
    // Dotted keys via SettingRegistry (e.g. "compression.threshold 0.8").
    if (arg.find('.') != std::string::npos) {
        size_t sp = arg.find(' ');
        std::string key = (sp == std::string::npos) ? arg : arg.substr(0, sp);
        std::string val = (sp == std::string::npos) ? "" : arg.substr(sp + 1);
        const Setting* s = settings_.find(key);
        if (s && s->setter && !val.empty()) {
            s->setter(std::string(val));
            append_line(P_STATUS, s->key + ": " + s->getter() + "  \u2014  " + s->help);
            cfg_.save_settings(settings_path_);
            return;
        }
    }
    if (arg.empty()) {
        append_line(P_STATUS, "detection loop: " + std::string(cfg_.detection_loop ? "on" : "off"));
        append_line(P_STATUS, "detection duplicate: " + std::string(cfg_.detection_duplicate ? "on" : "off"));
        append_line(P_STATUS, "display markdown: " + std::string(win().markdown_on ? "on" : "off"));
        append_line(P_STATUS, "toolfold: " + toolfold_name(tool_fold_));
        append_line(P_STATUS, "policy: " + mode_name(cfg_.mode));
        append_line(P_STATUS, "compression threshold: " +
            std::to_string(cfg_.compression_threshold > 0.0 ? cfg_.compression_threshold : 0.75));
        append_line(P_STATUS, "compression min_turns: " +
            std::to_string(cfg_.compression_min_turns_explicit ? cfg_.compression_min_turns : 10));
        append_line(P_STATUS, "provider: " + cfg_.provider_name);
        append_line(P_STATUS, "model: " + cfg_.model);
        append_line(P_STATUS, "thinking: " + cfg_.thinking);
        append_line(P_STATUS, "Use /set <option> <value> to change a setting");
        draw();
        return;
    }

    // policy: mode/approval/timeout/rule dispatch through the tree leaves
    // (core.config.set.policy.*); this branch only sees the bare namespace.
    if (arg.rfind("policy ", 0) == 0 || arg == "policy") {
        append_line(P_STATUS, "usage: /set policy mode <read|write|yolo> | /set policy rule <tool> <allow|deny|ask> | /set policy timeout <N> | /set policy approval <on|off>");
        draw();
        return;
    }

    // skills: export, enable|disable|block (documented verbs go to leaves)
    if (arg.rfind("skills ", 0) == 0 || arg == "skills") {
        std::string rest = (arg.size() > 7) ? arg.substr(7) : "";
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        cmd_skills_set(rest);
        return;
    }

    // provider switch
    if (arg.rfind("provider ", 0) == 0 || arg == "provider") {
        std::string rest = (arg.size() > 9) ? arg.substr(9) : "";
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        cmd_provider(rest);
        return;
    }

    append_line(P_STATUS, "unknown option: " + arg + " (try: detection, display, toolfold, policy, compression, provider, model, think, skills)");
}

void Tui::cmd_get(const std::string& arg) {
    // Namespace fallback: dotted keys, namespace expansions, learn/mcp
    // summaries, and the config screen.
    const Setting* reg = settings_.find(arg);
    if (reg && reg->getter) {
        std::string help_text = settings_.help_for(arg);
        std::string msg = arg + ": " + reg->getter();
        if (!help_text.empty()) msg += "  \u2014  " + help_text;
        append_line(P_STATUS, msg);
        return;
    }
    auto subs = settings_.keys_in(arg);
    if (!subs.empty()) {
        for (const auto& sub : subs) {
            std::string key = arg;
            key += ".";
            key += sub;
            cmd_get(key);
        }
        return;
    }
    if (arg == "learn" || arg == "learn ") {
        if (win().agent) {
            auto lines = agent::learn_summary_lines(
                win().agent->memory_store(), win().agent->experience_config());
            for (const auto& l : lines) append_line(P_STATUS, l);
        }
        draw();
        return;
    }
    if (arg == "mcp" || arg.rfind("mcp ", 0) == 0) {
        std::string sub = (arg.size() > 3) ? arg.substr(3) : "";
        if (sub.empty() || sub == " servers") {
            for (const auto& l : agent::mcp_list_lines(mcp_servers_))
                append_line(P_STATUS, l);
        } else if (sub == " prompts" || sub == ".prompts") {
            cmd_prompt_list();
            return;
        } else if (sub.rfind(" prompts ", 0) == 0 ||
                   sub.rfind(".prompts ", 0) == 0) {
            std::string server = sub.substr(sub.find(' ') + 1);
            const agent::MCPClient* c = mcp_servers_.client(server);
            if (!c) {
                append_line(P_STATUS, "server '" + server + "' not connected");
            } else {
                for (const auto& p : c->prompts())
                    append_line(P_STATUS, server + " \u00b7 " + p.name +
                                " \u00b7 " + p.description);
            }
        }
        draw();
        return;
    }
    config_screen();
    redraw_after_modal();
}

void Tui::cmd_get_config() {
    config_screen();
    redraw_after_modal();
}

void Tui::cmd_get_provider() {
    append_line(P_STATUS, "provider: " + cfg_.provider_name + " (" + cfg_.api_base + ")");
}

void Tui::cmd_get_toolfold() {
    append_line(P_STATUS, "toolfold: " + toolfold_name(tool_fold_));
}

void Tui::cmd_get_policy(const std::string& arg) {
    if (arg.empty()) {
        append_line(P_STATUS, "policy mode: " + mode_name(cfg_.mode));
        append_line(P_STATUS, "policy timeout: " + std::to_string(policy_timeout_) + "s");
        auto* ag = win().agent.get();
        if (ag) {
            for (const auto& r : ag->policy().rules()) {
                if (r.level == agent::PolicyLevel::Ask) continue;
                append_line(P_STATUS, "  " + r.tool + " \u2192 " +
                    agent::policy_level_name(r.level) +
                    " (used " + std::to_string(r.count) + "x)");
            }
        }
        return;
    }
    // Dotted or fallback form: "/get policy rule <tool>" arrives here only
    // when the tool is not a feed leaf.
    if (arg.rfind("rule", 0) == 0) {
        std::string name = arg;
        if (name.size() > 4 && name[4] == ' ') name = name.substr(5);
        else if (name.size() > 4) name = name.substr(4);
        cmd_get_policy_rule(name);
        return;
    }
    show_policy_rule(arg);
}

void Tui::show_policy_rule(const std::string& name) {
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
}

void Tui::cmd_get_policy_rule(const std::string& arg) {
    if (arg.empty()) {
        auto* ag = win().agent.get();
        if (!ag) return;
        bool any = false;
        for (const auto& r : ag->policy().rules()) {
            if (r.level == agent::PolicyLevel::Ask) continue;
            any = true;
            std::string line = "  " + r.tool + " \u2192 " +
                agent::policy_level_name(r.level);
            if (r.count > 0)
                line += " (used " + std::to_string(r.count) + "x)";
            append_line(P_STATUS, line);
        }
        if (!any)
            append_line(P_STATUS, "no stored rules \u2014 everything asks for approval");
        return;
    }
    show_policy_rule(arg);
}

void Tui::apply_policy_rule(const std::string& name, const std::string& lvl) {
    if (lvl.empty()) {
        show_policy_rule(name);
        return;
    }
    agent::PolicyLevel pl = agent::policy_level_from_name(lvl);
    if (pl == agent::PolicyLevel::Ask) {
        for (auto& w : windows_)
            if (w->agent) w->agent->policy().revoke(name);
        append_line(P_STATUS, "policy rule revoked for " + name);
    } else if (pl == agent::PolicyLevel::AlwaysAllow ||
               pl == agent::PolicyLevel::AlwaysDeny) {
        for (auto& w : windows_)
            if (w->agent) w->agent->policy().set_rule(name, pl);
        append_line(P_STATUS, "policy rule " + lvl + " for " + name);
    } else {
        append_line(P_STATUS, "invalid level: " + lvl + " (use allow, deny, or ask)");
        return;
    }
    if (win().agent) {
        std::string policy_path = agent::Workspace::local_dir() + "/policy.json";
        win().agent->policy().save(policy_path);
    }
    refresh_policy_feed();
    draw();
}

void Tui::cmd_set_policy_rule(const std::string& arg) {
    if (arg.empty()) {
        append_line(P_STATUS, "usage: /set policy rule <tool> <allow|deny|ask>");
        return;
    }
    size_t sp = arg.find(' ');
    std::string name = (sp == std::string::npos) ? arg : arg.substr(0, sp);
    std::string lvl = (sp == std::string::npos) ? "" : arg.substr(sp + 1);
    apply_policy_rule(name, lvl);
}

void Tui::refresh_policy_feed() {
    // Existing rules (union across windows): tool -> level info.
    std::map<std::string, std::string> rule_help;
    for (auto& w : windows_) {
        if (!w->agent) continue;
        for (const auto& r : w->agent->policy().rules()) {
            if (r.level == agent::PolicyLevel::Ask) continue;
            std::string info = agent::policy_level_name(r.level);
            if (r.count > 0)
                info += " (used " + std::to_string(r.count) + "x)";
            rule_help[r.tool] = info;
        }
    }
    // Set side: every registered tool (rule or not) is a value leaf; the
    // get side shows only tools that have a stored rule.
    std::set<std::string> tools;
    for (const auto& t : reg_.tools()) tools.insert(t->name());
    for (const auto& [tool, _] : rule_help) tools.insert(tool);

    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& tool : tools) {
        std::string info = rule_help.count(tool) ? rule_help.at(tool)
                                                 : "no rule (ask)";
        std::string action = "core.config.set.policy.rule." + tool;
        nlohmann::json& leaf =
            subtree["set"]["children"]["policy"]["children"]["rule"]["children"][tool];
        leaf["action"] = action;
        leaf["help"] = info;
        register_action(action, [this, tool](const std::string& a) {
            apply_policy_rule(tool, a);
        });
        if (rule_help.count(tool)) {
            std::string gaction = "core.config.get.policy.rule." + tool;
            nlohmann::json& g =
                subtree["get"]["children"]["policy"]["children"]["rule"]["children"][tool];
            g["action"] = gaction;
            g["help"] = info;
            register_action(gaction, [this, tool](const std::string&) {
                show_policy_rule(tool);
            });
        }
    }
    settings_.merge_completions_json(subtree);
}

void Tui::cmd_get_policy_mode() {
    append_line(P_STATUS, "policy mode: " + mode_name(cfg_.mode));
}

void Tui::cmd_get_policy_approval() {
    append_line(P_STATUS, "policy approval: " + std::string(cfg_.policy_approval ? "on" : "off"));
}

void Tui::cmd_get_policy_timeout() {
    append_line(P_STATUS, "policy timeout: " + std::to_string(policy_timeout_) + "s");
}

void Tui::cmd_get_display() {
    append_line(P_STATUS, "markdown: " + std::string(win().markdown_on ? "on" : "off"));
}

void Tui::cmd_get_think() {
    append_line(P_STATUS, "thinking: " + cfg_.thinking);
}

void Tui::cmd_get_detection(const std::string& sub) {
    if (sub.empty() || sub == "loop")
        append_line(P_STATUS, "detection loop: " + std::string(cfg_.detection_loop ? "on" : "off"));
    if (sub.empty() || sub == "duplicate")
        append_line(P_STATUS, "detection duplicate: " + std::string(cfg_.detection_duplicate ? "on" : "off"));
}

void Tui::cmd_get_compression() {
    double t = (cfg_.compression_threshold > 0.0) ? cfg_.compression_threshold : 0.75;
    int mt = cfg_.compression_min_turns_explicit ? cfg_.compression_min_turns : 10;
    append_line(P_STATUS, "compression threshold: " + std::to_string(t));
    append_line(P_STATUS, "compression min_turns: " + std::to_string(mt));
}

void Tui::cmd_skills_set(const std::string& rest) {
    if (!win().agent) {
        append_line(P_STATUS, "no agent in this window");
        draw();
        return;
    }
    agent::SkillCatalog& catalog = win().agent->skills();
    auto trim = [](const std::string& s) {
        size_t b = s.find_first_not_of(" \t");
        size_t e = s.find_last_not_of(" \t");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    // Namespace fallback: export / enable / disable / block + usage.
    std::string sub = trim(rest.substr(0, rest.find(' ')));
    std::string args = (rest.find(' ') == std::string::npos) ? "" : trim(rest.substr(rest.find(' ') + 1));
    if (sub == "export") {
        if (args.empty()) {
            append_line(P_STATUS, "usage: /set skills export <name>");
            draw();
            return;
        }
        std::string err = agent::skill_export(catalog, args);
        append_line(P_STATUS, err.empty()
            ? "exported '" + args + "' to global authored skills"
            : err);
        draw();
        return;
    }
    if (sub == "enable" || sub == "disable" || sub == "block") {
        if (args.empty()) {
            append_line(P_STATUS, "usage: /set skills " + sub + " <name>");
            draw();
            return;
        }
        std::string err = agent::skill_set_override(catalog, args, sub);
        append_line(P_STATUS, err.empty()
            ? "skill '" + args + "' " + sub + "d"
            : err);
        draw();
        return;
    }
    append_line(P_STATUS, "usage: /set skills interop on|off | refresh | show "
        "| create <name> [--global] | delete <name> [--global] | export <name> "
        "| install <path|url> | uninstall <name> | enable|disable|block <name>");
    draw();
}

void Tui::cmd_skills_interop(const std::string& val) {
    if (!win().agent) { append_line(P_STATUS, "no agent in this window"); return; }
    if (val != "on" && val != "off") {
        append_line(P_STATUS, "usage: /set skills interop on|off");
        return;
    }
    agent::SkillCatalog& catalog = win().agent->skills();
    cfg_.skills_interop = (val == "on");
    catalog.set_interop_enabled(val == "on");
    catalog.refresh();
    append_line(P_STATUS, "skills interop: " + val + " \u2014 .claude/skills and "
        ".codex/skills " + (val == "on" ? "scanned" : "ignored"));
    if (!cfg_.save_settings(settings_path_))
        append_line(P_STATUS, "warning: could not save to " + settings_path_);
    draw();
}

void Tui::cmd_skills_refresh() {
    if (!win().agent) { append_line(P_STATUS, "no agent in this window"); return; }
    win().agent->skills().refresh();
    append_line(P_STATUS, "skills refreshed");
    draw();
}

void Tui::cmd_skills_create(const std::string& args) {
    if (!win().agent) { append_line(P_STATUS, "no agent in this window"); return; }
    auto trim = [](const std::string& s) {
        size_t b = s.find_first_not_of(" \t");
        size_t e = s.find_last_not_of(" \t");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    std::string name = trim(args);
    std::string scope = "project";
    if (name.rfind("--global", 0) == 0) { scope = "global"; name.clear(); }
    else if (name.find(" --global") != std::string::npos) {
        scope = "global";
        name = trim(name.substr(0, name.find(" --global")));
    }
    if (name.empty()) {
        append_line(P_STATUS, "usage: /set skills create <name> [--global]");
        return;
    }
    std::string err = agent::skill_create(win().agent->skills(), name, name,
        "## " + name + "\n\n(instructions)", scope);
    append_line(P_STATUS, err.empty() ? ("skill '" + name + "' created (" + scope + ")") : err);
    draw();
}

void Tui::cmd_skills_delete(const std::string& args) {
    if (!win().agent) { append_line(P_STATUS, "no agent in this window"); return; }
    auto trim = [](const std::string& s) {
        size_t b = s.find_first_not_of(" \t");
        size_t e = s.find_last_not_of(" \t");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    std::string name = trim(args);
    std::string scope = "project";
    if (name.find(" --global") != std::string::npos) {
        scope = "global";
        name = trim(name.substr(0, name.find(" --global")));
    }
    if (name.empty()) {
        append_line(P_STATUS, "usage: /set skills delete <name> [--global]");
        return;
    }
    std::string err = agent::skill_delete(win().agent->skills(), name, scope);
    append_line(P_STATUS, err.empty() ? ("skill '" + name + "' deleted (" + scope + ")") : err);
    draw();
}

void Tui::cmd_skills_install(const std::string& source) {
    if (source.empty()) {
        append_line(P_STATUS, "usage: /set skills install <path|url>");
        return;
    }
    std::string global = agent::default_scan_paths().global;
    std::string err = agent::install_skill_pack(source, global);
    if (!err.empty()) { append_line(P_STATUS, "install failed: " + err); return; }
    if (win().agent) win().agent->skills().refresh();
    append_line(P_STATUS, "skill installed to " + global);
    draw();
}

void Tui::cmd_skills_uninstall(const std::string& name) {
    if (name.empty()) {
        append_line(P_STATUS, "usage: /set skills uninstall <name>");
        return;
    }
    std::string global = agent::default_scan_paths().global;
    std::string err = agent::uninstall_skill(name, global);
    if (!err.empty()) { append_line(P_STATUS, "uninstall failed: " + err); return; }
    if (win().agent) win().agent->skills().refresh();
    append_line(P_STATUS, "skill removed: " + name);
    draw();
}

void Tui::cmd_skills_get(const std::string& sub) {
    if (!win().agent) {
        append_line(P_STATUS, "no agent in this window");
        draw();
        return;
    }
    std::string name = sub;
    if (!name.empty() && name[0] == ' ') name = name.substr(1);
    auto lines = agent::skill_show_lines(win().agent->skills());
    bool any = false;
    for (const auto& l : lines) {
        if (!name.empty() && l.find(name) == std::string::npos) continue;
        any = true;
        append_line(P_STATUS, l);
    }
    if (!any)
        append_line(P_STATUS, name.empty() ? "(no skills)"
            : "no skill matching '" + name + "'");
    draw();
}

std::string Tui::plugin_state_name(agent::PluginState st) const {
    switch (st) {
    case agent::PluginState::Enabled: return "enabled";
    case agent::PluginState::Incompatible: return "incompatible";
    default: return "disabled";
    }
}

void Tui::cmd_plugin_list() {
    std::string msg;
    for (const auto& p : plugins_.plugins()) {
        msg += p.id + " v" + p.version + " [" + plugin_state_name(p.state) + "] ";
        if (!p.error.empty()) msg += "(" + p.error + ") ";
    }
    append_line(P_STATUS, "plugins: " + (msg.empty() ? "(none)" : msg));
}

void Tui::cmd_plugin_status(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /plugin status <id>"); return; }
    const agent::PluginInfo* p = plugins_.find(id);
    if (!p) { append_line(P_STATUS, "unknown plugin: " + id); return; }
    append_line(P_STATUS, id + " v" + p->version + " [" +
                              plugin_state_name(p->state) + "] dir " + p->dir);
    if (!p->error.empty()) append_line(P_STATUS, "  error: " + p->error);
}

void Tui::cmd_plugin_info(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /plugin info <id>"); return; }
    const agent::PluginInfo* p = plugins_.find(id);
    if (!p) { append_line(P_STATUS, "unknown plugin: " + id); return; }
    append_line(P_STATUS, id + " \u2014 " + p->manifest.name + " (" +
                              p->manifest.author + ")");
    append_line(P_STATUS, "  url: " + p->manifest.url);
    append_line(P_STATUS, "  license: " + p->manifest.license);
    append_line(P_STATUS, "  tools: " +
                              std::to_string(p->manifest.tools.size()));
}

void Tui::cmd_plugin_enable(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /plugin enable <id>"); return; }
    agent::PluginInfo* p = plugins_.find(id);
    if (!p) { append_line(P_STATUS, "unknown plugin: " + id); return; }
    if (plugins_.enable(id, reg_)) {
        refresh_completions();
        append_line(P_STATUS, "plugin enabled: " + id +
                                  " \u2014 tools are advertised in new conversations");
    } else {
        append_line(P_STATUS, "enable failed: " +
                                  (p->error.empty() ? std::string("unknown error") : p->error));
    }
}

void Tui::cmd_plugin_disable(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /plugin disable <id>"); return; }
    if (!plugins_.find(id)) { append_line(P_STATUS, "unknown plugin: " + id); return; }
    if (plugins_.disable(id, reg_)) {
        refresh_completions();
        append_line(P_STATUS, "plugin disabled: " + id);
    }
}

void Tui::cmd_plugin_get(const std::string& args) {
    size_t sp = args.find(' ');
    std::string id = (sp == std::string::npos) ? args : args.substr(0, sp);
    std::string key = (sp == std::string::npos) ? "" : args.substr(sp + 1);
    const agent::PluginInfo* p = plugins_.find(id);
    if (!p) { append_line(P_STATUS, "unknown plugin: " + id); return; }
    if (key.empty()) {
        std::string all;
        for (auto it = p->settings.begin(); it != p->settings.end(); ++it)
            all += it.key() + "=" + it.value().dump() + " ";
        append_line(P_STATUS, "settings " + id + ": " + all);
        return;
    }
    append_line(P_STATUS, id + " " + key + " = " + plugins_.get_setting(id, key));
}

void Tui::cmd_plugin_set(const std::string& args) {
    size_t sp = args.find(' ');
    if (sp == std::string::npos) {
        append_line(P_STATUS, "usage: /plugin set <id> <key>=<value>");
        return;
    }
    std::string id = args.substr(0, sp);
    std::string kv = args.substr(sp + 1);
    size_t eq = kv.find('=');
    if (eq == std::string::npos) {
        append_line(P_STATUS, "usage: /plugin set <id> <key>=<value>");
        return;
    }
    if (plugins_.set_setting(id, kv.substr(0, eq), kv.substr(eq + 1)))
        append_line(P_STATUS, "set " + id + " " + kv);
    else
        append_line(P_STATUS, "unknown plugin: " + id);
}

void Tui::cmd_plugin_install(const std::string& source) {
    if (source.empty()) { append_line(P_STATUS, "usage: /plugin install <path|url>"); return; }
    append_line(P_STATUS, "installing " + source + " ...");
    std::string err = plugins_.install(source);
    if (!err.empty()) { append_line(P_STATUS, "install failed: " + err); return; }
    plugins_.discover();
    refresh_completions();
    append_line(P_STATUS, "installed \u2014 /plugin enable <id> to activate");
}

void Tui::cmd_plugin_uninstall(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /plugin uninstall <id>"); return; }
    std::string err = plugins_.uninstall(id);
    if (!err.empty()) { append_line(P_STATUS, "uninstall failed: " + err); return; }
    plugins_.discover();
    refresh_completions();
    append_line(P_STATUS, "uninstalled: " + id);
}

void Tui::cmd_mcp(const std::string& rest) {
    // Namespace fallback: no documented verb matched (or empty).
    auto lines = agent::mcp_list_lines(mcp_servers_);
    if (lines.empty())
        append_line(P_STATUS, "(no MCP servers configured \u2014 see "
                    "~/.config/amber/mcp/<name>.conf)");
    for (const auto& l : lines) append_line(P_STATUS, l);
    if (!rest.empty())
        append_line(P_STATUS, "usage: /mcp list | show <server> | connect <server> | "
            "disconnect <server> | refresh <server> | prompts <server> | "
            "enable|disable <server> | trust <server> on|off");
    draw();
}

void Tui::cmd_mcp_show(const std::string& server) {
    if (server.empty()) { append_line(P_STATUS, "usage: /mcp show <server>"); draw(); return; }
    std::string err;
    auto lines = agent::mcp_show_lines(mcp_servers_, server, err);
    if (!err.empty()) append_line(P_STATUS, err);
    for (const auto& l : lines) append_line(P_STATUS, l);
    draw();
}

void Tui::cmd_mcp_connect(const std::string& server) {
    if (server.empty()) { append_line(P_STATUS, "usage: /mcp connect <server>"); draw(); return; }
    std::string err = agent::mcp_connect(mcp_servers_, reg_, server);
    append_line(P_STATUS, err.empty()
        ? ("mcp server '" + server + "' connected")
        : err);
    refresh_completions();
    draw();
}

void Tui::cmd_mcp_disconnect(const std::string& server) {
    if (server.empty()) { append_line(P_STATUS, "usage: /mcp disconnect <server>"); draw(); return; }
    agent::mcp_disconnect(mcp_servers_, reg_, server);
    append_line(P_STATUS, "mcp server '" + server + "' disconnected");
    refresh_completions();
    draw();
}

void Tui::cmd_mcp_refresh(const std::string& server) {
    if (server.empty()) { append_line(P_STATUS, "usage: /mcp refresh <server>"); draw(); return; }
    std::string err = agent::mcp_refresh(mcp_servers_, reg_, server);
    append_line(P_STATUS, err.empty()
        ? ("mcp server '" + server + "' refreshed")
        : err);
    refresh_completions();
    draw();
}

void Tui::cmd_mcp_prompts(const std::string& server) {
    if (server.empty()) { append_line(P_STATUS, "usage: /mcp prompts <server>"); draw(); return; }
    const agent::MCPClient* c = mcp_servers_.client(server);
    if (!c) {
        append_line(P_STATUS, "server '" + server + "' not connected");
    } else {
        for (const auto& p : c->prompts())
            append_line(P_STATUS, server + " \u00b7 " + p.name +
                        " \u00b7 " + p.description);
    }
    draw();
}

void Tui::cmd_mcp_set_enabled(const std::string& server, bool on) {
    if (server.empty()) {
        append_line(P_STATUS, "usage: /mcp " + std::string(on ? "enable" : "disable") + " <server>");
        draw();
        return;
    }
    std::string err = agent::mcp_enable(mcp_servers_, reg_, server, on);
    append_line(P_STATUS, err.empty()
        ? ("mcp server '" + server + "' " + (on ? "enabled" : "disabled"))
        : err);
    refresh_completions();
    draw();
}

void Tui::cmd_mcp_trust(const std::string& args) {
    size_t sp = args.find(' ');
    std::string server = (sp == std::string::npos) ? args : args.substr(0, sp);
    std::string val = (sp == std::string::npos) ? "" : args.substr(sp + 1);
    if (server.empty() || (val != "on" && val != "off")) {
        append_line(P_STATUS, "usage: /mcp trust <server> on|off");
        draw();
        return;
    }
    std::string err = agent::mcp_trust(mcp_servers_, server, val == "on");
    append_line(P_STATUS, err.empty()
        ? ("mcp server '" + server + "' trust: " + val)
        : err);
    draw();
}

void Tui::cmd_prompt_list() {
    bool any = false;
    for (const auto& st : mcp_servers_.snapshot()) {
        if (!st.connected) continue;
        const agent::MCPClient* c = mcp_servers_.client(st.name);
        if (!c) continue;
        for (const auto& p : c->prompts()) {
            any = true;
            append_line(P_STATUS, st.name + " \u00b7 " + p.name + " \u00b7 " +
                        p.description);
        }
    }
    if (!any) append_line(P_STATUS, "(no MCP prompts available)");
    draw();
}

void Tui::cmd_prompt(const std::string& rest) {
    if (rest.empty() || rest == "list") {
        cmd_prompt_list();
        return;
    }
    size_t sp = rest.find(' ');
    std::string server = (sp == std::string::npos) ? rest : rest.substr(0, sp);
    std::string tail = (sp == std::string::npos) ? "" : rest.substr(sp + 1);
    size_t sp2 = tail.find(' ');
    std::string name = (sp2 == std::string::npos) ? tail : tail.substr(0, sp2);
    std::string args_str = (sp2 == std::string::npos) ? "" : tail.substr(sp2 + 1);
    if (server.empty() || name.empty()) {
        append_line(P_STATUS, "usage: /prompt list | /prompt <server> <name> [k=v ...]");
        draw();
        return;
    }
    json arguments = json::object();
    std::stringstream ss(args_str);
    std::string kv;
    while (ss >> kv) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        arguments[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    std::string text;
    std::string err = agent::mcp_prompt(mcp_servers_, server, name,
                                        arguments, text);
    if (!err.empty()) {
        append_line(P_STATUS, err);
        draw();
        return;
    }
    input_fill_ = text;
    draw();
}


const std::vector<Command>& Tui::commands() {
    if (commands_.empty()) build_commands();
    return commands_;
}

void Tui::build_commands() {
    commands_ = {
        {"help", "core.help", {"?", "h"}, "[command]",
         "list commands, or show detail for one"},
        {"settings", "core.settings", {"server", "endpoint"}, "",
         "configure provider, model, API key, and connection test"},
        {"mcp", "core.mcp", {}, "[subcommand]",
         "manage MCP servers (list|show|connect|disconnect|refresh|prompts|enable|disable|trust)"},
        {"prompt", "core.prompt", {}, "[server name k=v...]",
         "invoke an MCP prompt template (fills the input line)"},
        {"plugin", "core.plugin", {}, "[subcommand]",
         "manage plugins (list|status|enable|disable|get|set|info|install|uninstall)"},
        {"new", "core.session.reset", {"clear", "reset"}, "",
         "clear the current conversation and start fresh"},
        {"close", "core.window.close", {}, "",
         "close the current window"},
        {"window", "core.window", {"win", "w"}, "new|close|list|rename <name>",
         "manage chat windows"},
        {"stop", "core.stop", {"cancel", "kill"}, "",
         "terminate the current tool and agent loop"},
        {"set", "core.config.set", {}, "<option> <value>",
         "set runtime options: detection, display, toolfold, policy (mode|tool <name> <level>|timeout <N>), compression, provider, model, think"},
        {"get", "core.config.get", {}, "<option>",
         "show current setting: config, model, provider, toolfold, policy (mode|timeout|<tool>), display, compression, detection"},
        {"compress", "core.compress", {"compact"}, "",
         "compress conversation history to free context space"},
        {"job", "core.job", {}, "[ls|kill <id>|read <id>|start <cmd>]",
         "manage background processes (servers, builds) started by the agent"},
{"save", "core.session.save", {}, "",
         "persist the current conversation"},
        {"sessions", "core.session.list", {"load", "open"}, "",
         "browse and load a saved session"},
        {"quit", "core.quit", {"exit", "q"}, "",
         "save all windows and exit"},
        {"provider", "core.provider", {"p"}, "list|add|edit|delete|test",
         "manage API providers"},
        {"session", "core.session", {}, "list|save|load|delete|rename <id> <title>",
         "manage saved sessions"},
        {"files", "core.files", {"f"}, "ls|tree|open|find <path>",
         "browse and view files in the workspace"},
        {"system", "core.system", {"sy"}, "exec|delete|rmdir|mkdir|mv|cp|info|ps|kill|df|uptime|uname",
         "system operations (file mgmt, processes, disk)"},
    };
    register_builtin_actions();
}

void Tui::register_action(const std::string& action,
                          std::function<void(const std::string&)> handler) {
    action_handlers_[action] = std::move(handler);
}

void Tui::register_builtin_actions() {
    register_action("core.help",
        [this](const std::string& a) { cmd_help(a); });
    register_action("core.settings", [this](const std::string&) {
        settings_screen(); redraw_after_modal(); });
    register_action("core.prompt", [this](const std::string& a) { cmd_prompt(a); });
    register_action("core.session.reset", [this](const std::string&) {
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
        append_line(P_STATUS, "conversation cleared \u2014 next message starts fresh");
        drawer_open_ = false;
        draw();
    });
    register_action("core.window.close", [this](const std::string&) { close_window(); });
    register_action("core.window.new", [this](const std::string&) { cmd_window_new(); });
    register_action("core.window.list", [this](const std::string&) { cmd_window_list(); });
    register_action("core.window.rename",
        [this](const std::string& a) { cmd_window_rename(a); });
    register_action("core.window", [this](const std::string& a) {
        if (!a.empty()) append_line(P_STATUS, "usage: /window new|close|list|rename <name>");
        cmd_window_list();
    });
    register_action("core.stop", [this](const std::string&) {
        cfg_.cancel_token.request();
        agent_cancel_.store(true);
        append_line(P_STATUS, "stop requested");
    });
    register_action("core.compress",
        [this](const std::string&) { cmd_compress(""); });
    register_action("core.job", [this](const std::string& a) { cmd_job(a); });
    register_action("core.job.list", [this](const std::string&) { job_ls(); });
    register_action("core.job.kill", [this](const std::string& a) { job_kill(a); });
    register_action("core.job.read", [this](const std::string& a) { job_read(a); });
    register_action("core.job.start", [this](const std::string& a) { job_start(a); });
    register_action("core.session.save",
        [this](const std::string&) { save_session(); });
    register_action("core.session.list",
        [this](const std::string&) { session_browser(); });
    register_action("core.session.load",
        [this](const std::string& a) { cmd_session_load(a); });
    register_action("core.session.delete",
        [this](const std::string& a) { cmd_session_delete(a); });
    register_action("core.session.rename", [this](const std::string&) {
        append_line(P_STATUS, "rename not yet implemented");
    });
    register_action("core.session", [this](const std::string& a) {
        if (!a.empty())
            append_line(P_STATUS, "usage: /session list|save|load|delete|rename <id> <title>");
        session_browser();
    });
    register_action("core.quit",
        [this](const std::string&) { request_quit(); });
    // provider
    register_action("core.provider",
        [this](const std::string& a) { cmd_provider(a); });
    register_action("core.provider.list",
        [this](const std::string&) { cmd_provider_list(); });
    register_action("core.provider.add", [this](const std::string&) {
        append_line(P_STATUS, "use /settings to add/edit providers");
    });
    register_action("core.provider.edit", [this](const std::string&) {
        append_line(P_STATUS, "use /settings to add/edit providers");
    });
    register_action("core.provider.delete",
        [this](const std::string& a) { cmd_provider_delete(a); });
    register_action("core.provider.test",
        [this](const std::string& a) { cmd_provider_test(a); });
    // model (get/set accessor — see completions.json get.model/set.model)
    // files
    register_action("core.files", [this](const std::string& a) {
        if (!a.empty())
            append_line(P_STATUS, "usage: /files ls|tree|open|find <path>");
        cmd_files_ls("");
    });
    register_action("core.files.ls",
        [this](const std::string& a) { cmd_files_ls(a); });
    register_action("core.files.tree",
        [this](const std::string& a) { cmd_files_tree(a); });
    register_action("core.files.open",
        [this](const std::string& a) { cmd_files_open(a); });
    register_action("core.files.find",
        [this](const std::string& a) { cmd_files_find(a); });
    // system
    register_action("core.system", [this](const std::string& a) {
        if (!a.empty())
            append_line(P_STATUS, "usage: /system exec|delete|rmdir|mkdir|mv|cp|info|ps|kill|df|uptime|uname");
        append_line(P_STATUS, "system operations: /system exec <cmd> | ps | df | uptime | uname | kill <pid> | info <path>");
    });
    register_action("core.system.exec",
        [this](const std::string& a) { cmd_system_exec(a); });
    register_action("core.system.delete",
        [this](const std::string& a) { cmd_system_delete(a); });
    register_action("core.system.rmdir",
        [this](const std::string& a) { cmd_system_rmdir(a); });
    register_action("core.system.mkdir",
        [this](const std::string& a) { cmd_system_mkdir(a); });
    register_action("core.system.mv",
        [this](const std::string& a) { cmd_system_mv(a); });
    register_action("core.system.cp",
        [this](const std::string& a) { cmd_system_cp(a); });
    register_action("core.system.info",
        [this](const std::string& a) { cmd_system_info(a); });
    register_action("core.system.ps",
        [this](const std::string&) { cmd_system_ps(); });
    register_action("core.system.kill",
        [this](const std::string& a) { cmd_system_kill(a); });
    register_action("core.system.df",
        [this](const std::string&) { cmd_system_df(); });
    register_action("core.system.uptime",
        [this](const std::string&) { cmd_system_uptime(); });
    register_action("core.system.uname",
        [this](const std::string&) { cmd_system_uname(); });
    // set namespace + children
    register_action("core.config.set", [this](const std::string& a) { cmd_set(a); });
    register_action("core.config.set.detection.loop", [this](const std::string& v) {
        cmd_set_detection_toggle("loop", v);
    });
    register_action("core.config.set.detection.duplicate", [this](const std::string& v) {
        cmd_set_detection_toggle("duplicate", v);
    });
    register_action("core.config.set.subagent.parallel", [this](const std::string& v) {
        cmd_set_subagent_parallel(v);
    });
    register_action("core.config.set.subagent.max", [this](const std::string& v) {
        cmd_set_subagent_max(v);
    });
    register_action("core.config.set.display.markdown", [this](const std::string& v) {
        if (v != "on" && v != "off") {
            append_line(P_STATUS, "usage: /set display markdown on|off");
            return;
        }
        win().markdown_on = (v == "on");
        append_line(P_STATUS, "markdown rendering: " + v);
        draw();
    });
    register_action("core.config.set.toolfold", [this](const std::string& v) {
        if (v == "always") tool_fold_ = ToolFold::Always;
        else if (v == "auto") tool_fold_ = ToolFold::Auto;
        else if (v == "never") tool_fold_ = ToolFold::Never;
        else { append_line(P_STATUS, "usage: /set toolfold always|auto|never"); return; }
        append_line(P_STATUS, "tool fold: " + v);
        draw();
    });
    register_action("core.config.set.policy.mode", [this](const std::string& v) {
        if (v != "read" && v != "write" && v != "yolo") {
            append_line(P_STATUS, "usage: /set policy mode read|write|yolo");
            return;
        }
        if (v == "read") cfg_.mode = agent::AgentMode::Read;
        else if (v == "yolo") cfg_.mode = agent::AgentMode::Yolo;
        else cfg_.mode = agent::AgentMode::Write;
        append_line(P_STATUS, "policy mode: " + v);
        draw();
    });
    register_action("core.config.set.policy.approval", [this](const std::string& v) {
        if (v != "on" && v != "off") {
            append_line(P_STATUS, "usage: /set policy approval on|off");
            return;
        }
        cfg_.policy_approval = (v == "on");
        append_line(P_STATUS, "policy approval: " + v);
    });
    register_action("core.config.set.policy.timeout", [this](const std::string& v) {
        int n = std::atoi(v.c_str());
        if (v.empty() || n < 0) {
            append_line(P_STATUS, "usage: /set policy timeout <N>");
            return;
        }
        policy_timeout_ = n;
        append_line(P_STATUS, "policy timeout: " + std::to_string(n) + "s");
    });
    register_action("core.config.set.policy", [this](const std::string& a) { cmd_set(a); });
    register_action("core.config.set.policy.rule",
        [this](const std::string& a) { cmd_set_policy_rule(a); });
    register_action("core.config.set.compression.threshold", [this](const std::string& v) {
        double t = std::atof(v.c_str());
        if (t <= 0.0 || t > 1.0 || v.empty()) {
            append_line(P_STATUS, "usage: /set compression threshold <0.1-1.0>");
            return;
        }
        cfg_.compression_threshold = t;
        cfg_.compression_threshold_explicit = true;
        for (auto& w : windows_)
            if (w->agent) w->agent->set_compression_threshold(t);
        append_line(P_STATUS, "compression threshold: " + std::to_string(t));
        if (!cfg_.save_settings(settings_path_))
            append_line(P_STATUS, "warning: could not save to " + settings_path_);
        draw();
    });
    register_action("core.config.set.compression.min_turns", [this](const std::string& v) {
        int n = std::atoi(v.c_str());
        if (v.empty()) {
            append_line(P_STATUS, "usage: /set compression min_turns <0-999> (0 = disabled)");
            return;
        }
        cfg_.compression_min_turns = n;
        cfg_.compression_min_turns_explicit = true;
        for (auto& w : windows_)
            if (w->agent) w->agent->set_compression_min_turns(n);
        append_line(P_STATUS, "compression min_turns: " + std::to_string(n));
        if (!cfg_.save_settings(settings_path_))
            append_line(P_STATUS, "warning: could not save to " + settings_path_);
        draw();
    });
    register_action("core.config.set.compression", [this](const std::string& a) { cmd_set(a); });
    register_action("core.config.set.think", [this](const std::string& v) {
        if (v != "on" && v != "off" && v != "auto") {
            append_line(P_STATUS, "usage: /set think on|off|auto");
            return;
        }
        cfg_.thinking = v;
        append_line(P_STATUS, "thinking: " + v);
    });
    register_action("core.config.set.skills", [this](const std::string& a) { cmd_skills_set(a); });
    register_action("core.config.set.skills.interop",
        [this](const std::string& v) { cmd_skills_interop(v); });
    register_action("core.config.set.skills.refresh",
        [this](const std::string&) { cmd_skills_refresh(); });
    register_action("core.config.set.skills.create",
        [this](const std::string& a) { cmd_skills_create(a); });
    register_action("core.config.set.skills.delete",
        [this](const std::string& a) { cmd_skills_delete(a); });
    register_action("core.config.set.skills.install",
        [this](const std::string& a) { cmd_skills_install(a); });
    register_action("core.config.set.skills.uninstall",
        [this](const std::string& a) { cmd_skills_uninstall(a); });
    // get namespace + children
    register_action("core.config.get", [this](const std::string& a) { cmd_get(a); });
    register_action("core.config.get.config",
        [this](const std::string&) { cmd_get_config(); });
    register_action("core.config.get.model",
        [this](const std::string&) { cmd_get_model(); });
    register_action("core.config.get.model.list",
        [this](const std::string&) { cmd_get_model_list(); });
    register_action("core.config.get.model.context",
        [this](const std::string&) { cmd_get_model_context(); });
    register_action("core.config.set.model",
        [this](const std::string& a) { cmd_model_set(a); });
    register_action("core.config.get.provider",
        [this](const std::string&) { cmd_get_provider(); });
    register_action("core.config.get.toolfold",
        [this](const std::string&) { cmd_get_toolfold(); });
    register_action("core.config.get.policy",
        [this](const std::string& a) { cmd_get_policy(a); });
    register_action("core.config.get.policy.mode",
        [this](const std::string&) { cmd_get_policy_mode(); });
    register_action("core.config.get.policy.approval",
        [this](const std::string&) { cmd_get_policy_approval(); });
    register_action("core.config.get.policy.timeout",
        [this](const std::string&) { cmd_get_policy_timeout(); });
    register_action("core.config.get.policy.rule",
        [this](const std::string& a) { cmd_get_policy_rule(a); });
    register_action("core.config.get.display",
        [this](const std::string&) { cmd_get_display(); });
    register_action("core.config.get.think",
        [this](const std::string&) { cmd_get_think(); });
    register_action("core.config.get.detection",
        [this](const std::string& a) { cmd_get_detection(a); });
    register_action("core.config.get.detection.loop",
        [this](const std::string&) { cmd_get_detection("loop"); });
    register_action("core.config.get.detection.duplicate",
        [this](const std::string&) { cmd_get_detection("duplicate"); });
    register_action("core.config.get.subagent",
        [this](const std::string&) { cmd_get_subagent(); });
    register_action("core.config.set.reasoning.effort", [this](const std::string& v) {
        cmd_set_reasoning_effort(v);
    });
    register_action("core.config.get.reasoning",
        [this](const std::string&) { cmd_get_reasoning(); });
    register_action("core.config.get.compression",
        [this](const std::string&) { cmd_get_compression(); });
    register_action("core.config.get.skills",
        [this](const std::string& a) { cmd_skills_get(a); });
    register_action("core.config.get.skills.show",
        [this](const std::string&) { cmd_skills_get("show"); });
    // mcp
    register_action("core.mcp", [this](const std::string& a) { cmd_mcp(a); });
    register_action("core.mcp.list", [this](const std::string&) { cmd_mcp(""); });
    register_action("core.mcp.show",
        [this](const std::string& a) { cmd_mcp_show(a); });
    register_action("core.mcp.connect",
        [this](const std::string& a) { cmd_mcp_connect(a); });
    register_action("core.mcp.disconnect",
        [this](const std::string& a) { cmd_mcp_disconnect(a); });
    register_action("core.mcp.refresh",
        [this](const std::string& a) { cmd_mcp_refresh(a); });
    register_action("core.mcp.prompts",
        [this](const std::string& a) { cmd_mcp_prompts(a); });
    register_action("core.mcp.enable",
        [this](const std::string& a) { cmd_mcp_set_enabled(a, true); });
    register_action("core.mcp.disable",
        [this](const std::string& a) { cmd_mcp_set_enabled(a, false); });
    register_action("core.mcp.trust",
        [this](const std::string& a) { cmd_mcp_trust(a); });
    // plugin
    register_action("core.plugin", [this](const std::string& a) {
        if (!a.empty())
            append_line(P_STATUS, "usage: /plugin list|status|enable|disable|get|set|info|install|uninstall");
        cmd_plugin_list();
    });
    register_action("core.plugin.list",
        [this](const std::string&) { cmd_plugin_list(); });
    register_action("core.plugin.status",
        [this](const std::string& a) { cmd_plugin_status(a); });
    register_action("core.plugin.enable",
        [this](const std::string& a) { cmd_plugin_enable(a); });
    register_action("core.plugin.disable",
        [this](const std::string& a) { cmd_plugin_disable(a); });
    register_action("core.plugin.get",
        [this](const std::string& a) { cmd_plugin_get(a); });
    register_action("core.plugin.set",
        [this](const std::string& a) { cmd_plugin_set(a); });
    register_action("core.plugin.info",
        [this](const std::string& a) { cmd_plugin_info(a); });
    register_action("core.plugin.install",
        [this](const std::string& a) { cmd_plugin_install(a); });
    register_action("core.plugin.uninstall",
        [this](const std::string& a) { cmd_plugin_uninstall(a); });
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

    // Tokenize.
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : rest) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) tokens.push_back(cur);
    }
    if (tokens.empty()) return true;

    // Walk the command tree, consuming every token that names a documented
    // child. The deepest node with a registered action handler receives the
    // remaining text — command structure lives in the tree, not in handlers.
    const json& tree = settings_.command_tree();
    const json* node = nullptr;
    const json* children = nullptr;
    if (tree.contains("commands") && tree["commands"].is_object())
        children = &tree["commands"];
    size_t consumed = 0;
    while (consumed < tokens.size() && children && children->is_object()) {
        auto it = children->find(tokens[consumed]);
        if (it == children->end() || !it->is_object()) break;
        node = &*it;
        ++consumed;
        children = (node->contains("children") && (*node)["children"].is_object())
                       ? &(*node)["children"]
                       : nullptr;
    }
    if (!node) {
        append_line(P_STATUS,
                    "unknown command: /" + tokens[0] + "  (try /help)");
        return true;
    }
    std::string action;
    if (node->contains("action") && (*node)["action"].is_string())
        action = (*node)["action"].get<std::string>();
    std::string arg;
    for (size_t i = consumed; i < tokens.size(); ++i) {
        if (!arg.empty()) arg += " ";
        arg += tokens[i];
    }

    auto it = action_handlers_.find(action);
    if (it == action_handlers_.end()) {
        // Documented in the tree but no handler: show the node's manual page.
        if (node->contains("man") && (*node)["man"].is_string()) {
            append_line(P_STATUS, "/" + tokens[0] + ": " +
                                      (*node)["man"].get<std::string>());
        } else {
            append_line(P_STATUS, "/" + tokens[0] + ": no handler for this action");
        }
        return true;
    }
    it->second(arg);
    return true;
}


std::string Tui::usage(const Command& c) const {
    return palette::usage(c);
}

void Tui::cmd_model_set(const std::string& arg) {
    if (arg.empty()) {
        append_line(P_STATUS, "model: " + cfg_.model + " \u2014 /model <name> or /set model to switch");
        return;
    }
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
        // The running agent's LLM client holds a config snapshot — rebuild it
        // so the next turn actually talks to the new model.
        w->agent->set_model(arg);
    }
    std::string global = agent::global_config_path();
    cfg_.save_global(global);
    append_line(P_STATUS, "model set to " + arg + " (saved to " + global + ")");
}

void Tui::cmd_get_model() {
    append_line(P_STATUS, "model: " + cfg_.model + " (provider: " + cfg_.provider_name + ")");
}

void Tui::cmd_get_model_list() {
    refresh_model_list();
    if (model_info_.empty()) {
        append_line(P_STATUS, "no models available or server unreachable");
        return;
    }
    for (const auto& m : model_info_) {
        int ctx = m.context ? m.context : m.context_train;
        std::string line = "  " + m.id;
        if (ctx > 0) line += "  (ctx " + std::to_string(ctx) + ")";
        append_line(P_ASSISTANT, line);
    }
}

void Tui::cmd_get_model_context() {
    agent::ServerInfo info = agent::probe_server(cfg_);
    std::string line = "model: " + cfg_.model;
    if (info.ok && info.context_size > 0) {
        line += "  ctx: " + std::to_string(info.context_size);
        if (info.context_train > 0 && info.context_train != info.context_size)
            line += " (max " + std::to_string(info.context_train) + ")";
    } else if (cfg_.context_size > 0) {
        line += "  ctx: " + std::to_string(cfg_.context_size) + " (cached)";
    } else {
        line += "  ctx: unknown";
    }
    append_line(P_STATUS, line);
}

void Tui::refresh_model_list() {
    model_info_ = agent::list_model_info(cfg_);
    // Feed: model ids become value leaves under set.model. Each leaf carries a
    // generated action (core.config.set.model.<id>) so the tree walk resolves
    // the exact model; the closure runs the shared set/validate/save path.
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& m : model_info_) {
        std::string id = m.id;
        nlohmann::json& leaf = subtree["set"]["children"]["model"]["children"][id];
        leaf["action"] = "core.config.set.model." + id;
        int ctx = m.context ? m.context : m.context_train;
        if (ctx > 0) leaf["help"] = "ctx " + std::to_string(ctx);
        register_action(leaf["action"].get<std::string>(),
                        [this, id](const std::string&) { cmd_model_set(id); });
    }
    settings_.merge_completions_json(subtree);
}

void Tui::cmd_provider(const std::string& a) {
    if (a.empty()) {
        append_line(P_STATUS, "current provider: " + cfg_.provider_name +
                     " (" + cfg_.api_base + ")");
        return;
    }
    if (!agent::is_known_provider(a)) {
        append_line(P_STATUS, "unknown provider: " + a +
                     " (try: openrouter, kilocode, custom, or add a "
                     "provider file under ~/.config/amber/providers/)");
        return;
    }
    const auto* prov = agent::provider::find(a);
    if (a == "custom") {
        append_line(P_STATUS, "provider set to custom (use /set or amber.conf to configure)");
        return;
    }
    cfg_.apply_provider(a);
    if (prov && prov->requires_key && cfg_.api_key.empty()) {
        append_line(P_STATUS, "warning: " + a + " requires an API key (set AMBER_API_KEY)");
    }
    std::string global = agent::global_config_path();
    cfg_.save_global(global);
    append_line(P_STATUS, "provider switched to " + a + " (saved to " + global + ")");
}

void Tui::cmd_provider_list() {
    auto providers = agent::list_saved_providers();
    std::string msg;
    for (auto& p : providers) msg += p + " ";
    append_line(P_STATUS, "providers: " + (msg.empty() ? "(none)" : msg));
}

void Tui::cmd_provider_delete(const std::string& name) {
    if (name.empty()) { append_line(P_STATUS, "usage: /provider delete <name>"); return; }
    agent::delete_provider(name);
    append_line(P_STATUS, "deleted provider: " + name);
}

void Tui::cmd_provider_test(const std::string& name) {
    if (name.empty()) { append_line(P_STATUS, "usage: /provider test <name>"); return; }
    append_line(P_STATUS, "testing " + name + "...");
    agent::Config test_cfg = cfg_;
    agent::load_provider(name, test_cfg);
    agent::ServerInfo info = agent::probe_server(test_cfg);
    append_line(P_STATUS, name + ": " + (info.ok ? "OK" : "FAILED"));
}

void Tui::cmd_session_load(const std::string& id) {
    if (id.empty()) { session_browser(); return; }
    agent::Session sess;
    store_.load(id, sess);
    if (sess.id.empty())
        append_line(P_STATUS, "session not found: " + id);
    else
        load_session(id);
}

void Tui::cmd_session_delete(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /session delete <id>"); return; }
    store_.remove(id);
    append_line(P_STATUS, "deleted session: " + id);
}

void Tui::cmd_files_ls(const std::string& rest) {
    namespace fs = std::filesystem;
    std::string root = agent::Workspace::root();
    std::string path = rest.empty() ? "." : rest;
    if (path[0] != '/') path = root + "/" + path;
    fs::path p(path);
    if (!fs::exists(p)) { append_line(P_STATUS, "not found: " + path); return; }
    if (fs::is_directory(p)) {
        std::string out;
        for (const auto& e : fs::directory_iterator(p))
            out += e.path().filename().string() + "  ";
        if (out.empty()) out = "(empty)";
        append_line(P_ASSISTANT, out);
    } else {
        append_line(P_ASSISTANT, p.filename().string());
    }
}

void Tui::cmd_files_tree(const std::string& rest) {
    namespace fs = std::filesystem;
    std::string root = agent::Workspace::root();
    std::string path = rest.empty() ? "." : rest;
    if (path[0] != '/') path = root + "/" + path;
    fs::path p(path);
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
}

void Tui::cmd_files_open(const std::string& rest) {
    namespace fs = std::filesystem;
    std::string root = agent::Workspace::root();
    std::string path = rest.empty() ? "." : rest;
    if (path[0] != '/') path = root + "/" + path;
    fs::path p(path);
    if (!fs::exists(p) || fs::is_directory(p)) {
        append_line(P_STATUS, "not a file: " + path);
        return;
    }
    std::ifstream f(p);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    if (content.size() > 4096) content.resize(4096);
    append_line(P_ASSISTANT, p.filename().string() + ":\n" + content);
}

void Tui::cmd_files_find(const std::string& rest) {
    namespace fs = std::filesystem;
    std::string root = agent::Workspace::root();
    std::string path = rest.empty() ? "." : rest;
    if (path[0] != '/') path = root + "/" + path;
    fs::path p(path);
    if (!fs::exists(p) || !fs::is_directory(p)) {
        append_line(P_STATUS, "not a directory: " + path);
        return;
    }
    for (const auto& e : fs::directory_iterator(p))
        append_line(P_ASSISTANT, e.path().filename().string());
}

void Tui::cmd_system_exec(const std::string& rest) {
    if (rest.empty()) { append_line(P_STATUS, "usage: /system exec <command>"); return; }
    auto run_cmd = [&](const std::string& cmd) -> std::string {
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return "(popen failed)";
        std::string out;
        char buf[4096];
        while (fgets(buf, sizeof buf, f)) out += buf;
        pclose(f);
        if (out.size() > 4096) out.resize(4096);
        return out;
    };
    append_line(P_ASSISTANT, run_cmd(rest));
}

void Tui::cmd_system_delete(const std::string& rest) {
    if (rest.empty()) { append_line(P_STATUS, "usage: /system delete <path>"); return; }
    std::string resolved, err;
    if (!agent::Workspace::confine(rest, resolved, err)) {
        append_line(P_STATUS, "delete denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(resolved, ec);
    append_line(P_STATUS, ec ? ("delete failed: " + ec.message()) : ("deleted: " + rest));
}

void Tui::cmd_system_rmdir(const std::string& rest) {
    if (rest.empty()) { append_line(P_STATUS, "usage: /system rmdir <path>"); return; }
    std::string resolved, err;
    if (!agent::Workspace::confine(rest, resolved, err)) {
        append_line(P_STATUS, "rmdir denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::remove(resolved, ec);
    append_line(P_STATUS, ec ? ("rmdir failed: " + ec.message()) : ("removed dir: " + rest));
}

void Tui::cmd_system_mkdir(const std::string& rest) {
    if (rest.empty()) { append_line(P_STATUS, "usage: /system mkdir <path>"); return; }
    std::string resolved, err;
    if (!agent::Workspace::confine(rest, resolved, err)) {
        append_line(P_STATUS, "mkdir denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(resolved, ec);
    append_line(P_STATUS, ec ? ("mkdir failed: " + ec.message()) : ("created: " + rest));
}

void Tui::cmd_system_mv(const std::string& rest) {
    if (rest.empty()) { append_line(P_STATUS, "usage: /system mv <src> <dst>"); return; }
    size_t sp = rest.find(' ');
    if (sp == std::string::npos) { append_line(P_STATUS, "usage: /system mv <src> <dst>"); return; }
    std::string src, dst, err;
    if (!agent::Workspace::confine(rest.substr(0, sp), src, err) ||
        !agent::Workspace::confine(rest.substr(sp + 1), dst, err)) {
        append_line(P_STATUS, "mv denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::rename(src, dst, ec);
    append_line(P_STATUS, ec ? ("mv failed: " + ec.message()) : "moved");
}

void Tui::cmd_system_cp(const std::string& rest) {
    if (rest.empty()) { append_line(P_STATUS, "usage: /system cp <src> <dst>"); return; }
    size_t sp = rest.find(' ');
    if (sp == std::string::npos) { append_line(P_STATUS, "usage: /system cp <src> <dst>"); return; }
    std::string src, dst, err;
    if (!agent::Workspace::confine(rest.substr(0, sp), src, err) ||
        !agent::Workspace::confine(rest.substr(sp + 1), dst, err)) {
        append_line(P_STATUS, "cp denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive, ec);
    append_line(P_STATUS, ec ? ("cp failed: " + ec.message()) : "copied");
}

void Tui::cmd_system_info(const std::string& rest) {
    if (rest.empty()) { append_line(P_STATUS, "usage: /system info <path>"); return; }
    std::string resolved, err;
    if (!agent::Workspace::confine(rest, resolved, err)) {
        append_line(P_STATUS, "info denied: " + err);
        return;
    }
    std::error_code ec;
    auto st = std::filesystem::status(resolved, ec);
    if (ec) { append_line(P_STATUS, "info failed: " + ec.message()); return; }
    append_line(P_STATUS, rest + ": " + (std::filesystem::is_directory(st) ? "dir" : "file"));
}

void Tui::cmd_system_ps() {
    FILE* f = popen("ps -eo pid,comm,args --no-headers | head -30", "r");
    if (!f) return;
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    append_line(P_ASSISTANT, out.empty() ? "(no processes)" : out);
}

void Tui::cmd_system_kill(const std::string& rest) {
    if (rest.empty()) { append_line(P_STATUS, "usage: /system kill <pid>"); return; }
    auto pid = static_cast<pid_t>(std::atoi(rest.c_str()));
    if (pid <= 0) { append_line(P_STATUS, "invalid pid: " + rest); return; }
    append_line(P_STATUS, kill(pid, SIGKILL) == 0 ? ("killed " + rest) : "kill failed");
}

void Tui::cmd_system_df() {
    FILE* f = popen("df -h | head -20", "r");
    if (!f) return;
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    append_line(P_ASSISTANT, out.empty() ? "(no output)" : out);
}

void Tui::cmd_system_uptime() {
    FILE* f = popen("uptime", "r");
    if (!f) return;
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    append_line(P_ASSISTANT, out.empty() ? "(no output)" : out);
}

void Tui::cmd_system_uname() {
    FILE* f = popen("uname -a", "r");
    if (!f) return;
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    append_line(P_ASSISTANT, out.empty() ? "(no output)" : out);
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

void Tui::cmd_window_new() { new_window("chat"); draw(); }
void Tui::cmd_window_close() { close_window(); }
void Tui::cmd_window_list() {
    std::string s = "windows:";
    for (size_t i = 0; i < windows_.size(); ++i)
        s += " " + std::to_string(i + 1) + ":" + windows_[i]->title +
             (i == active_ ? "*" : "");
    append_line(P_STATUS, s);
}
void Tui::cmd_window_rename(const std::string& name) {
    if (name.empty()) { append_line(P_STATUS, "usage: /window rename <name>"); return; }
    win().title = name;
    append_line(P_STATUS, "renamed window to " + win().title);
    draw();
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

void Tui::cmd_job(const std::string& rest) {
    if (!rest.empty())
        append_line(P_STATUS, "usage: /job [ls|kill <id>|read <id>|start <cmd>]");
    job_ls();
}

void Tui::job_ls() {
    auto jobs = jobs_.list();
    if (jobs.empty()) { append_line(P_STATUS, "no background jobs"); return; }
    for (const auto& j : jobs) append_line(P_STATUS, job_list_line(j));
}

void Tui::refresh_job_feed() {
    // Job ids become value leaves under job.kill / job.read (state as help),
    // so /job kill|read complete through the tree like every other branch.
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& j : jobs_.list()) {
        std::string id = j.id;
        nlohmann::json& kill_leaf =
            subtree["job"]["children"]["kill"]["children"][id];
        kill_leaf["action"] = "core.job.kill." + id;
        kill_leaf["help"] = job_state_name(j.state);
        nlohmann::json& read_leaf =
            subtree["job"]["children"]["read"]["children"][id];
        read_leaf["action"] = "core.job.read." + id;
        read_leaf["help"] = job_state_name(j.state);
        register_action("core.job.kill." + id,
                        [this, id](const std::string&) { job_kill(id); });
        register_action("core.job.read." + id,
                        [this, id](const std::string&) { job_read(id); });
    }
    settings_.merge_completions_json(subtree);
}

void Tui::job_kill(const std::string& id) {
    if (id.empty()) { append_line(P_STATUS, "usage: /job kill <id>"); return; }
    bool ok = jobs_.stop(id);
    append_line(P_STATUS, ok ? ("killed " + id) : ("no such job: " + id));
    refresh_job_feed();
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
    refresh_job_feed();
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
    prov_display.emplace_back("  + Add new provider...");
    prov_id.emplace_back("");  // sentinel

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
            std::string line = prefix;
            line += id;
            line += "  (";
            if (id == "openrouter" || id == "kilocode" || id == "custom") {
                line += key_hint;
            } else {
                line += cfg_.api_key.empty() && active ? "no-key" : "key-set";
            }
            line += ")";
            rich_display.push_back(line);
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
        actions.emplace_back("Delete provider");
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
        settings_.add({key, help, placeholder, type, std::move(choices),
                       rmin, rmax, std::move(getter), std::move(setter)});
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
    add("reasoning.effort", "Reasoning effort", "<off|low|medium|high>", Setting::Choice,
        {"off","low","medium","high"}, 0, 0,
        [this](){ return cfg_.reasoning_effort; },
        [this](const std::string& v) {
            if (v != "off" && v != "low" && v != "medium" && v != "high") return;
            cfg_.reasoning_effort = v;
            for (auto& w : windows_)
                if (w->agent) w->agent->set_reasoning_effort(v);
            cfg_.save_settings(settings_path_);
        });
    add("subagent.parallel", "Sub-agent parallelism", "<on|off|toggle>", Setting::Choice,
        {"on","off","toggle"}, 0, 0,
        [this](){ return subagents_.parallel() ? "on" : "off"; },
        [this](const std::string& v) {
            if (v == "toggle") subagents_.set_parallel(!subagents_.parallel());
            else subagents_.set_parallel(v == "on");
            cfg_.subagent_parallel = subagents_.parallel();
            cfg_.save_settings(settings_path_);
        });
    add("subagent.max", "Max concurrent sub-agents", "<1-16>", Setting::Int,
        {}, 1, 16,
        [this](){ return std::to_string(subagents_.max()); },
        [this](const std::string& v) {
            int n = std::atoi(v.c_str());
            if (n < 1 || n > 16) return;
            subagents_.set_max(n);
            cfg_.subagent_max = n;
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
        [this]() -> std::string { return toolfold_name(tool_fold_); },
        [this](const std::string& v) {
            if (v == "always") tool_fold_ = ToolFold::Always;
            else if (v == "never") tool_fold_ = ToolFold::Never;
            else tool_fold_ = ToolFold::Auto;
            cfg_.save_settings(settings_path_);
        });
    add("policy.mode", "Agent mode", "<read|write|yolo>", Setting::Choice,
        {"read","write","yolo"}, 0, 0,
        [this]() -> std::string { return mode_name(cfg_.mode); },
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


#include "tui.h"
#include "usage_report.h"

#include <agent/compact.h>
#include "feed_manager.h"
#include "tui/list_panel.h"
#include "tui/model_picker.h"
#include "tui/confirm_panel.h"
#include "tui/path_confine.h"
#include "tui/toggle_value.h"
#include "tui/window_ops.h"
#include "tui/widgets.h"
#include "tui/dialog.h"
#include "tui/drawer_rows.h"
#include "plugins/watch/watch_plugin.h"
#include "agent/model_probe.h"
#include "agent/provider_health.h"
#include "agent/config_paths.h"
#include "agent/data_path.h"
#include "agent/skill_shelves.h"
#include "agent/plugin_console.h"
#include "agent/plugin_runtime.h"
#include "agent/skill_cli.h"
#include "agent/skill_commands.h"
#include "agent/skill_install.h"
#include "agent/skill_index.h"
#include "agent/skill_ledger.h"
#include "tui/skills_explorer.h"
#include "agent/mcp_commands.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <agent/job.h>
#include <agent/workspace.h>
#include <agent/worktree.h>

namespace tui {
// Provider edit form (Server URL / API Key / Model). Returns true when the
// user saved, false on cancel. The context window is a deliberate pin, set
// with /set model context, not a provider-form field (a provider default that
// an existing pin outranks was the bug in #290).
static bool edit_provider_form(agent::Config& cfg, const std::string& title);

namespace {

std::string mode_name(agent::AgentMode m) {
    if (m == agent::AgentMode::Read)
        return "read";
    if (m == agent::AgentMode::Yolo)
        return "yolo";
    return "write";
}

struct ModelChoice {
    bool found = false;
    agent::ModelCapabilities caps;
};

std::vector<std::string> split_words(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> out;
    std::string word;
    while (in >> word)
        out.push_back(word);
    return out;
}

const char* worktree_state_label(agent::WorktreeState state) {
    switch (state) {
    case agent::WorktreeState::Live:
        return "live";
    case agent::WorktreeState::Parked:
        return "parked";
    case agent::WorktreeState::Kept:
        return "kept";
    }
    return "?";
}

// The model row also carries the limits/reasoning the listing advertised; the
// request must respect them, so the pick hands them to the agent.
ModelChoice find_model(const std::vector<agent::ModelInfo>& models, const std::string& id) {
    for (const auto& m : models) {
        if (m.id != id)
            continue;
        agent::ModelCapabilities caps = agent::capabilities_of(m);
        if (caps.context == 0)
            caps.context = m.context_train;
        return {true, std::move(caps)};
    }
    return {};
}

} // namespace

void Tui::fold_reasoning(Window& w) {
    std::string summary = w.reason.fold();
    if (!summary.empty())
        append_line_to(w, P_REASONING, summary);
}

void Tui::flush_stream(Window& w) {
    fold_reasoning(w);
    if (w.stream_buf.empty())
        return;
    // Commit the streamed reply through the Markdown renderer so headings,
    // code fences, lists, etc. survive into the scrollback (the live preview
    // in draw() already renders it as Markdown).
    append_markdown(w, w.stream_buf);
    w.stream_buf.clear();
    w.stream_ts.clear();
    draw();
}

void SlashDispatcher::cmd_set_detection_toggle(const std::string& key, const std::string& val) {
    if (!valid_toggle(val)) {
        tui_.append_line(P_STATUS,
                         "usage: /set detection " + key + " off|on|toggle (got: " + val + ")");
        return;
    }
    // One predicate, four uses below; testing the key again at each one cost a branch
    // and made it possible to read the hint for one setting over a value set on another.
    const bool loop = key == "loop";
    bool* field = loop ? &tui_.cfg_.detection_loop : &tui_.cfg_.detection_duplicate;
    const bool new_val = parse_toggle(val, *field);
    *field = new_val;
    if (auto& agent = tui_.win().agent) {
        if (loop)
            agent->set_detection_loop(new_val);
        else
            agent->set_detection_duplicate(new_val);
    }
    const std::string hint = loop ? (new_val ? "breaks on repeat" : "runs until stop")
                                  : (new_val ? "rejects duplicates" : "may repeat calls");
    tui_.append_line(P_STATUS,
                     "detection " + key + ": " + (new_val ? "on" : "off") + " \u2014 " + hint);
    if (!tui_.cfg_.save_settings(tui_.session_controller_->settings_path()))
        tui_.append_line(P_STATUS,
                         "warning: could not save to " + tui_.session_controller_->settings_path());
    tui_.draw();
}

void SlashDispatcher::cmd_set_subagent_parallel(const std::string& val) {
    if (!valid_toggle(val)) {
        tui_.append_line(P_STATUS,
                         "usage: /set subagent parallel on|off|toggle (got: " + val + ")");
        return;
    }
    bool new_val;
    if (val == "on")
        new_val = true;
    else if (val == "off")
        new_val = false;
    else
        new_val = !tui_.subagents_.parallel();
    tui_.subagents_.set_parallel(new_val);
    tui_.cfg_.subagent_parallel = new_val;
    tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
    tui_.append_line(P_STATUS, std::string("subagent parallel: ") + (new_val ? "on" : "off") +
                                   " \u2014 " +
                                   (new_val ? "concurrent workers" : "serial (cache-friendly)"));
    tui_.draw();
}

void SlashDispatcher::cmd_set_subagent_max(const std::string& val) {
    int n = 0;
    try {
        n = std::stoi(val);
    } catch (...) {
        n = -1;
    }
    if (n < 1 || n > 16) {
        tui_.append_line(P_STATUS, "usage: /set subagent max <1-16> (got: " + val + ")");
        return;
    }
    tui_.subagents_.set_max(n);
    tui_.cfg_.subagent_max = n;
    tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
    tui_.append_line(P_STATUS, "subagent max: " + std::to_string(n));
    tui_.draw();
}

void SlashDispatcher::cmd_get_subagent() {
    tui_.append_line(P_STATUS, std::string("subagent parallel: ") +
                                   (tui_.subagents_.parallel() ? "on" : "off") +
                                   ", max: " + std::to_string(tui_.subagents_.max()));
    tui_.draw();
}

bool SlashDispatcher::busy_reject(const std::string& what) {
    // Only the ACTIVE window's run gates settings — a sibling's agent
    // running in the background must never lock this window's commands.
    if (!tui_.runs_.busy(tui_.win().id))
        return false;
    tui_.append_line(P_STATUS, what + ": agent is busy \u2014 apply when idle");
    return true;
}

void SlashDispatcher::cmd_set_reasoning_effort(const std::string& val) {
    if (busy_reject("reasoning effort"))
        return;

    if (val != "off" && val != "low" && val != "medium" && val != "high") {
        tui_.append_line(P_STATUS,
                         "usage: /set reasoning effort off|low|medium|high (got: " + val + ")");
        return;
    }
    tui_.cfg_.reasoning_effort = val;
    if (tui_.win().agent)
        tui_.win().agent->set_reasoning_effort(val);
    tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
    tui_.append_line(P_STATUS, "reasoning effort: " + val + " (applies from the next turn)");
    tui_.draw();
}

void SlashDispatcher::cmd_get_reasoning() {
    tui_.append_line(P_STATUS, "reasoning effort: " + tui_.cfg_.reasoning_effort);
    tui_.draw();
}

// /set <dotted.key> <value>: route through the SettingRegistry. Returns true
// when the key existed and the value was applied (or the setter reported its own
// error); false lets the caller try the other /set forms.
bool SlashDispatcher::apply_setting_key(const std::string& arg) {
    const size_t sp = arg.find(' ');
    const std::string key = (sp == std::string::npos) ? arg : arg.substr(0, sp);
    const std::string val = (sp == std::string::npos) ? "" : arg.substr(sp + 1);
    const Setting* s = tui_.settings_.find(key);
    if (!s || !s->setter || val.empty())
        return false;
    try {
        s->setter(std::string(val));
    } catch (const std::exception& e) {
        tui_.append_line(P_STATUS, s->key + ": " + e.what());
        return true;
    }
    tui_.append_line(P_STATUS,
                     s->key + ": " + s->getter() + "  \u2014  " + tui_.settings_.help_for(s->key));
    tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
    return true;
}

// The bare /set overview: the current value of every documented option.
void SlashDispatcher::print_set_overview() {
    const auto& cfg = tui_.cfg_;
    tui_.append_line(P_STATUS, "detection loop: " + std::string(cfg.detection_loop ? "on" : "off"));
    tui_.append_line(P_STATUS,
                     "detection duplicate: " + std::string(cfg.detection_duplicate ? "on" : "off"));
    tui_.append_line(P_STATUS,
                     "display markdown: " + std::string(tui_.win().markdown_on ? "on" : "off"));
    tui_.append_line(P_STATUS, "policy: " + mode_name(cfg.mode));
    tui_.append_line(P_STATUS,
                     "compression threshold: " + std::to_string(compression_threshold_effective()));
    tui_.append_line(P_STATUS,
                     "compression min_turns: " + std::to_string(cfg.compression_min_turns_explicit
                                                                    ? cfg.compression_min_turns
                                                                    : 10));
    tui_.append_line(P_STATUS, "provider: " + cfg.provider_name);
    tui_.append_line(P_STATUS, "model: " + cfg.model);
    tui_.append_line(P_STATUS, "thinking: " + cfg.thinking);
    tui_.append_line(P_STATUS, "Use /set <option> <value> to change a setting");
    tui_.draw();
}

// The /set policy namespace page. Registered as the namespace's own action, so
// no handler has to recognise the name: the tree already knows which node was
// typed.
void SlashDispatcher::cmd_set_policy_usage() {
    tui_.append_line(P_STATUS,
                     "usage: /set policy mode <read|write|yolo> | /set policy rule <tool> "
                     "<allow|deny|ask> | /set policy timeout <N> | /set policy approval <on|off>");
    tui_.draw();
}

// Unknown option: the hint is derived from the tree, never hardcoded.
void SlashDispatcher::print_unknown_setting_hint(const std::string& arg) {
    std::string hint;
    for (const auto& child : tui_.settings_.children_of("set")) {
        if (!hint.empty())
            hint += ", ";
        hint += child;
    }
    tui_.append_line(P_STATUS,
                     "unknown option: " + arg + (hint.empty() ? "" : " (try: " + hint + ")"));
}

void SlashDispatcher::cmd_set(const std::string& arg) {
    // Dotted keys via SettingRegistry (e.g. "compression.threshold 0.8").
    if (arg.find('.') != std::string::npos && apply_setting_key(arg))
        return;
    if (arg.empty()) {
        print_set_overview();
        return;
    }
    print_unknown_setting_hint(arg);
}

void SlashDispatcher::cmd_get(const std::string& arg) {
    // Namespace fallback: dotted keys, namespace expansions, learn/mcp
    // summaries, and the config screen.
    const Setting* reg = tui_.settings_.find(arg);
    if (reg && reg->getter) {
        std::string help_text = tui_.settings_.help_for(arg);
        std::string msg = arg + ": " + reg->getter();
        if (!help_text.empty())
            msg += "  \u2014  " + help_text;
        tui_.append_line(P_STATUS, msg);
        return;
    }
    auto subs = tui_.settings_.keys_in(arg);
    if (!subs.empty()) {
        for (const auto& sub : subs) {
            std::string key = arg;
            key += '.';
            key += sub;
            cmd_get(key);
        }
        return;
    }
    if (arg == "learn" || arg == "learn ") {
        if (tui_.win().agent) {
            auto lines = agent::learn_summary_lines(tui_.win().agent->memory_store(),
                                                    tui_.win().agent->experience_config());
            for (const auto& l : lines)
                tui_.append_line(P_STATUS, l);
        }
        tui_.draw();
        return;
    }
    tui_.config_screen();
    tui_.redraw_after_modal();
}

void SlashDispatcher::cmd_get_config() {
    tui_.config_screen();
    tui_.redraw_after_modal();
}

// Where amber keeps its own files, resolved (XDG overrides included). One line
// per entry, so the readout stays greppable, and the same table the bundled
// amber-config skill renders its body from -- the two cannot describe
// different files.
void SlashDispatcher::cmd_get_config_paths() {
    for (const auto& p : agent::config_paths()) {
        std::string line = "  " + p.what;
        if (line.size() < 18)
            line += std::string(18 - line.size(), ' ');
        else
            line += "  ";
        tui_.append_line(P_STATUS, line + p.path);
    }
}

void SlashDispatcher::cmd_get_provider() {
    tui_.append_line(P_STATUS,
                     "provider: " + tui_.cfg_.provider_name + " (" + tui_.cfg_.api_base + ")");
}

void SlashDispatcher::cmd_get_policy(const std::string& arg) {
    if (arg.empty()) {
        tui_.append_line(P_STATUS, "policy mode: " + mode_name(tui_.cfg_.mode));
        tui_.append_line(P_STATUS, "policy timeout: " + std::to_string(tui_.policy_timeout_) + "s");
        auto* ag = tui_.win().agent.get();
        if (ag) {
            for (const auto& r : ag->policy().rules()) {
                if (r.level == agent::PolicyLevel::Ask)
                    continue;
                tui_.append_line(P_STATUS, "  " + agent::scope_display(r) + " \u2192 " +
                                               agent::policy_level_name(r.level) + " (used " +
                                               std::to_string(r.count) + "x)");
            }
        }
        return;
    }
    show_policy_rule(arg);
}

// The completion feed shows dotted names ("bash.rm", "outside./etc"); the
// policy store keys on scope ids ("bash:rm", "outside:/etc"). Translate a
// user-facing name into its scope id, mapping the FIRST '.' after the tool
// back to ':'; everything after it stays literal.
std::string scope_from_display(const std::string& name) {
    std::size_t dot = name.find('.');
    if (dot == std::string::npos)
        return name;
    std::string scope = name;
    scope[dot] = ':';
    return scope;
}

void SlashDispatcher::show_policy_rule(const std::string& name) {
    if (auto* ag = tui_.win().agent.get()) {
        const auto* r = ag->policy().find(scope_from_display(name));
        if (r) {
            tui_.append_line(P_STATUS, "rule " + name + ": " + agent::policy_level_name(r->level) +
                                           " (last: " + agent::policy_level_name(r->last_choice) +
                                           ", used " + std::to_string(r->count) + "x)");
        } else {
            tui_.append_line(P_STATUS, "rule " + name + ": ask (no stored rule)");
        }
    }
}

void SlashDispatcher::cmd_get_policy_rule(const std::string& arg) {
    if (arg.empty()) {
        auto* ag = tui_.win().agent.get();
        if (!ag)
            return;
        bool any = false;
        for (const auto& r : ag->policy().rules()) {
            if (r.level == agent::PolicyLevel::Ask)
                continue;
            any = true;
            std::string line =
                "  " + agent::scope_display(r) + " \u2192 " + agent::policy_level_name(r.level);
            if (r.count > 0)
                line += " (used " + std::to_string(r.count) + "x)";
            tui_.append_line(P_STATUS, line);
        }
        if (!any)
            tui_.append_line(P_STATUS, "no stored rules \u2014 everything asks for approval");
        return;
    }
    show_policy_rule(arg);
}

void SlashDispatcher::apply_policy_rule(const std::string& name, const std::string& lvl) {
    if (lvl.empty()) {
        show_policy_rule(name);
        return;
    }
    std::string scope = scope_from_display(name);
    agent::PolicyLevel pl = agent::policy_level_from_name(lvl);
    if (pl == agent::PolicyLevel::Ask) {
        if (tui_.win().agent)
            tui_.win().agent->policy().revoke(scope);
        tui_.append_line(P_STATUS, "policy rule revoked for " + name);
    } else if (pl == agent::PolicyLevel::AlwaysAllow || pl == agent::PolicyLevel::AlwaysDeny) {
        if (tui_.win().agent)
            tui_.win().agent->policy().set_rule(scope, pl);
        tui_.append_line(P_STATUS, "policy rule " + lvl + " for " + name);
    } else {
        tui_.append_line(P_STATUS, "invalid level: " + lvl + " (use allow, deny, or ask)");
        return;
    }
    if (tui_.win().agent) {
        std::string policy_path = agent::Workspace::policy_path();
        tui_.win().agent->policy().save(policy_path);
    }
    refresh_policy_feed();
    tui_.draw();
}

void SlashDispatcher::cmd_set_policy_rule(const std::string& arg) {
    if (arg.empty()) {
        tui_.append_line(P_STATUS, "usage: /set policy rule <tool> <allow|deny|ask>");
        return;
    }
    size_t sp = arg.find(' ');
    std::string name = (sp == std::string::npos) ? arg : arg.substr(0, sp);
    std::string lvl = (sp == std::string::npos) ? "" : arg.substr(sp + 1);
    apply_policy_rule(name, lvl);
}

void SlashDispatcher::refresh_provider_feed() {
    if (tui_.feed_manager_)
        tui_.feed_manager_->refresh_provider_feed();
}

void SlashDispatcher::refresh_policy_feed() {
    if (tui_.feed_manager_)
        tui_.feed_manager_->refresh_policy_feed();
}

void SlashDispatcher::cmd_get_policy_mode() {
    tui_.append_line(P_STATUS, "policy mode: " + mode_name(tui_.cfg_.mode));
}

void SlashDispatcher::cmd_get_policy_approval() {
    tui_.append_line(P_STATUS,
                     "policy approval: " + std::string(tui_.cfg_.policy_approval ? "on" : "off"));
}

void SlashDispatcher::cmd_get_policy_timeout() {
    tui_.append_line(P_STATUS, "policy timeout: " + std::to_string(tui_.policy_timeout_) + "s");
}

void SlashDispatcher::cmd_get_display() {
    tui_.append_line(P_STATUS, "markdown: " + std::string(tui_.win().markdown_on ? "on" : "off"));
}

void SlashDispatcher::cmd_get_think() {
    tui_.append_line(P_STATUS, "thinking: " + tui_.cfg_.thinking);
}

void SlashDispatcher::cmd_get_detection(const std::string& sub) {
    if (sub.empty() || sub == "loop")
        tui_.append_line(P_STATUS,
                         "detection loop: " + std::string(tui_.cfg_.detection_loop ? "on" : "off"));
    if (sub.empty() || sub == "duplicate")
        tui_.append_line(P_STATUS, "detection duplicate: " +
                                       std::string(tui_.cfg_.detection_duplicate ? "on" : "off"));
}

void SlashDispatcher::cmd_get_compression() {
    auto cc = agent::load_compression_config(tui_.cfg_);
    double t = compression_threshold_effective();
    tui_.append_line(P_STATUS, "compression threshold: " + std::to_string(t));
    tui_.append_line(P_STATUS, "compression min_turns: " + std::to_string(cc.min_turns));
    tui_.append_line(P_STATUS, "compression target_pct: " + std::to_string(cc.target_pct));
    tui_.append_line(P_STATUS,
                     "compression keep_last_prompts: " + std::to_string(cc.keep_last_prompts));
}

namespace {

std::vector<std::string> skill_sources_lines() {
    const agent::SkillSources sources = agent::skill_sources(false);
    return {"index       " + sources.index_base + " (unauthenticated search)",
            "github api  " + sources.github_api, "github raw  " + sources.github_raw,
            "cache       " + sources.cache_dir + " (" + sources.cache_summary + ")"};
}

// "/skills install <target> [--global]" -> target + scope.
struct InstallArg {
    std::string target;
    bool global = false;
};

InstallArg split_install_arg(const std::string& arg) {
    InstallArg out;
    out.target = arg;
    const std::string flag = " --global";
    if (out.target.size() > flag.size() &&
        out.target.compare(out.target.size() - flag.size(), flag.size(), flag) == 0) {
        out.global = true;
        out.target.resize(out.target.size() - flag.size());
    }
    while (!out.target.empty() && out.target.back() == ' ')
        out.target.pop_back();
    return out;
}

// The explorer's verbs, wired as one unit so the dispatcher keeps a single
// method for the whole skills domain.
void wire_skills_explorer_verbs(
    tui::ActionRegistry& registry, const std::function<void(tui::SkillsExplorerOptions)>& run,
    const std::function<tui::SkillsExplorerOptions(const std::string&)>& options,
    const std::function<bool()>& ready) {
    for (const char* action : {"core.skills", "core.skills.browse"})
        registry.register_action(action, [run, options](const std::string& a) { run(options(a)); });
    registry.register_action("core.skills.search", [run, options, ready](const std::string& a) {
        if (!ready())
            return;
        tui::SkillsExplorerOptions opts = options("");
        opts.initial_query = a;
        run(std::move(opts));
    });
    registry.register_action("core.skills.install", [run, options, ready](const std::string& a) {
        if (!ready())
            return;
        const InstallArg parsed = split_install_arg(a);
        tui::SkillsExplorerOptions opts = options("");
        opts.install_id = parsed.target;
        opts.install_global = parsed.global;
        run(std::move(opts));
    });
    registry.register_action("core.skills.update",
                             [run, options](const std::string&) { run(options("installed")); });
    registry.register_action("core.skills.remove", [run, options, ready](const std::string& a) {
        if (!ready())
            return;
        tui::SkillsExplorerOptions opts = options("installed");
        opts.initial_filter = a;
        run(std::move(opts));
    });
}

// The readout verbs share the /set skills listing instead of duplicating it.
void wire_skills_readouts(tui::ActionRegistry& registry,
                          const std::function<std::vector<std::string>()>& list,
                          const std::function<void(const std::string&)>& append,
                          const std::function<void(const std::string&)>& show,
                          const std::function<void()>& sources) {
    registry.register_action("core.skills.list", [list, append](const std::string&) {
        for (const auto& line : list())
            append(line);
    });
    registry.register_action("core.skills.show", [show](const std::string& a) { show(a); });
    registry.register_action("core.skills.sources", [sources](const std::string&) { sources(); });
}

const char* kSkillsUsage = "usage: /set skills interop on|off | refresh | show "
                           "| create <name> [--global] | delete <name> [--global] | export <name> "
                           "| install <path|url> | uninstall <name> | enable|disable|block <name>";

// Split "/set skills <sub> <args...>", trimming both sides of each.
void split_skills_rest(const std::string& rest, std::string& sub, std::string& args) {
    auto trim = [](const std::string& s) {
        const size_t b = s.find_first_not_of(" \t");
        const size_t e = s.find_last_not_of(" \t");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    const size_t sp = rest.find(' ');
    sub = trim(rest.substr(0, sp));
    args = (sp == std::string::npos) ? "" : trim(rest.substr(sp + 1));
}

std::string skills_export_line(agent::SkillCatalog& catalog, const std::string& args) {
    if (args.empty())
        return "usage: /set skills export <name>";
    const std::string err = agent::skill_export(catalog, args);
    return err.empty() ? "exported '" + args + "' to global authored skills" : err;
}

std::string skills_override_line(agent::SkillCatalog& catalog, const std::string& args,
                                 const std::string& sub) {
    if (args.empty())
        return "usage: /set skills " + sub + " <name>";
    const std::string err = agent::skill_set_override(catalog, args, sub);
    return err.empty() ? "skill '" + args + "' " + sub + "d" : err;
}

} // namespace

std::map<std::string, agent::LedgerEntry> skill_ledger_entries(const agent::SkillScanPaths& paths) {
    std::map<std::string, agent::LedgerEntry> entries;
    for (const auto& root : {paths.project, paths.global}) {
        agent::SkillLedger ledger = agent::SkillLedger::for_root(root);
        ledger.load();
        for (const auto& [name, entry] : ledger.entries())
            entries.emplace(name, entry);
    }
    return entries;
}

std::string skill_budget_text(const agent::SkillCatalog& catalog) {
    const auto discovery = catalog.discovery_block();
    std::size_t chars = 0;
    for (const auto& line : discovery)
        chars += line.size();
    return std::to_string(discovery.size()) + " lines · ~" + std::to_string(chars / 4) +
           " tok/turn";
}

std::vector<tui::SkillRow>
installed_skill_rows(const agent::SkillCatalog& catalog,
                     const std::map<std::string, agent::LedgerEntry>& ledger) {
    std::vector<tui::SkillRow> rows;
    std::set<std::string> seen;
    for (const auto& entry : catalog.entries()) {
        tui::SkillRow row;
        row.ref.id = entry.path.empty() ? entry.name : entry.path;
        row.ref.name = entry.name;
        row.ref.source = agent::skill_scope_name(entry.scope);
        row.description = entry.meta.description;
        row.state = entry.state;
        row.installed = true;
        row.unmanaged = ledger.count(entry.name) == 0;
        seen.insert(entry.name);
        rows.push_back(std::move(row));
    }
    // The ledger is the acquisition record: a package installed moments ago
    // belongs on the shelf even if this window's catalog snapshot has not
    // rescanned yet. It is also the only source for the install path, which is
    // what remove and update need.
    const agent::SkillScanPaths paths = agent::default_scan_paths();
    for (const auto& [root, scope] :
         {std::pair{paths.project, "project"}, std::pair{paths.global, "global"}}) {
        agent::SkillLedger installed = agent::SkillLedger::for_root(root);
        installed.load();
        for (const auto& [name, entry] : installed.entries()) {
            if (seen.count(name) != 0)
                continue;
            tui::SkillRow row;
            row.ref.id = root;
            row.ref.id += "/";
            row.ref.id += name;
            row.ref.name = name;
            row.ref.source = scope;
            row.state = entry.enabled ? "enabled" : "disabled";
            row.installed = true;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

// Acquisition settings (spec §7) and their one-session environment
// overrides, applied to the explorer's options.
void apply_skill_source_options(const agent::Config& cfg, tui::SkillsExplorerOptions& opts) {
    // Acquisition settings (spec §7), with the environment as a one-session
    // override. The two token names are configurable; an empty value keeps the
    // default variable.
    opts.review_required = cfg.skills_review_required;
    opts.audit_required = cfg.skills_audit_required;
    opts.install_global = cfg.skills_install_scope == "global";
    if (cfg.skills_index == "none")
        opts.index_disabled = true;
    else if (cfg.skills_index != "skills.sh")
        opts.index_base = cfg.skills_index;
    if (const char* token = std::getenv(cfg.skills_index_token_env.c_str()))
        opts.token = token;
    else if (const char* fallback = std::getenv("SKILLS_SH_TOKEN"))
        opts.token = fallback;
    if (const char* token = std::getenv(cfg.skills_github_token_env.c_str()))
        opts.github_token = token;
    if (const char* base = std::getenv("AMBER_SKILLS_INDEX_URL")) {
        opts.index_base = base;
        opts.index_disabled = false;
    }
    if (const char* api = std::getenv("AMBER_GITHUB_API_URL"))
        opts.github_api_base = api;
    if (const char* raw = std::getenv("AMBER_GITHUB_RAW_URL"))
        opts.github_raw_base = raw;
}

// The bundled shelf tables, resolved through the same data-path rules as every
// other data file. A missing or invalid table leaves the shelf empty; the
// loader records the reason and the explorer simply has nothing to show.
void load_skill_shelves(tui::SkillsExplorerOptions& opts) {
    std::string error;
    const std::string signals = agent::resolve_data_path("data/project-signals.json", nullptr);
    if (!signals.empty())
        opts.project_signals = agent::load_project_signals(signals, error);
    const std::string collections =
        agent::resolve_data_path("data/skill-collections.json", nullptr);
    if (!collections.empty())
        opts.collections = agent::load_collections(collections, error);
    const std::string owners = agent::resolve_data_path("data/skill-owners.json", nullptr);
    if (owners.empty())
        return;
    for (const auto& owner : agent::load_curated_owners(owners, error))
        opts.owners.push_back(owner.name);
}

void Tui::configure_skills_explorer(tui::SkillsExplorerOptions& opts, const std::string& arg) {
    opts.rows = LINES;
    opts.cols = COLS;
    const agent::SkillScanPaths paths = agent::default_scan_paths();
    opts.repo_root = agent::Workspace::project_root();
    opts.project_root = paths.project;
    opts.global_root = paths.global;
    opts.cache_root = agent::skills_index_cache_dir();
    opts.ui_state_path = agent::Workspace::state_dir() + "/skills-ui.json";
    // Shelf data. A missing or invalid file is reported once, here, and the
    // shelf simply has nothing to show.
    load_skill_shelves(opts);
    apply_skill_source_options(cfg_, opts);
    if (arg == "installed")
        opts.initial_tab = tui::SkillTab::Installed;
    const auto ledger = skill_ledger_entries(paths);
    opts.ledger = [ledger](const std::string& name) -> std::optional<agent::LedgerEntry> {
        const auto it = ledger.find(name);
        return it == ledger.end() ? std::nullopt : std::optional<agent::LedgerEntry>(it->second);
    };
    opts.set_state = [this](const std::string& name, const std::string& state) {
        return win().agent->skills().apply_override(name, state);
    };
    opts.is_active = [this](const std::string& name) {
        const auto active = win().agent->activated_skills();
        return std::any_of(
            active.begin(), active.end(),
            [&name](const agent::ActivatedSkill& skill) { return skill.name == name; });
    };
    opts.budget = skill_budget_text(win().agent->skills());
    opts.body_budget_tokens =
        cfg_.skills_body_budget_tokens > 0 ? cfg_.skills_body_budget_tokens : 5000;
    opts.installed = [this, ledger] { return installed_skill_rows(win().agent->skills(), ledger); };
}

void Tui::open_skills_explorer(const std::string& arg) {
    if (!win().agent) {
        append_line(P_STATUS, "no agent in this window");
        return;
    }
    // Reflect what is on disk now: a skill installed from this explorer (or by
    // a pack/CLI in the same session) must appear on the Installed shelf
    // without a restart.
    win().agent->skills().refresh();
    tui::SkillsExplorerOptions opts;
    configure_skills_explorer(opts, arg);
    tui::run_skills_explorer(opts,
                             [this](const std::string& line) { append_line(P_STATUS, line); });
    redraw_after_modal();
}

void SlashDispatcher::cmd_skills_set(const std::string& rest) {
    std::string sub;
    std::string args;
    split_skills_rest(rest, sub, args);
    // Acquisition settings are host-level: they apply before a window exists
    // and do not depend on the catalog.
    if (sub == "review" || sub == "audit" || sub == "scope" || sub == "index" ||
        sub == "update-review" || sub == "update-check") {
        std::string message;
        if (!agent::apply_skill_acquisition_setting(tui_.cfg_, sub, args, message)) {
            tui_.append_line(P_STATUS, message);
        } else {
            tui_.append_line(P_STATUS, message);
            if (!tui_.cfg_.save_settings(tui_.session_controller_->settings_path()))
                tui_.append_line(P_STATUS, "warning: could not save " +
                                               tui_.session_controller_->settings_path());
        }
        tui_.draw();
        return;
    }
    if (!tui_.win().agent) {
        tui_.append_line(P_STATUS, "no agent in this window");
        tui_.draw();
        return;
    }
    agent::SkillCatalog& catalog = tui_.win().agent->skills();

    // Namespace fallback: export / enable / disable / block + usage.
    if (sub == "export")
        tui_.append_line(P_STATUS, skills_export_line(catalog, args));
    else if (sub == "enable" || sub == "disable" || sub == "block")
        tui_.append_line(P_STATUS, skills_override_line(catalog, args, sub));
    else
        tui_.append_line(P_STATUS, kSkillsUsage);
    tui_.draw();
}

void SlashDispatcher::cmd_skills_interop(const std::string& val) {
    if (!tui_.win().agent) {
        tui_.append_line(P_STATUS, "no agent in this window");
        return;
    }
    if (val != "on" && val != "off") {
        tui_.append_line(P_STATUS, "usage: /set skills interop on|off");
        return;
    }
    agent::SkillCatalog& catalog = tui_.win().agent->skills();
    tui_.cfg_.skills_interop = (val == "on");
    catalog.set_interop_enabled(val == "on");
    catalog.refresh();
    tui_.append_line(P_STATUS, "skills interop: " + val +
                                   " \u2014 .claude/skills and "
                                   ".codex/skills " +
                                   (val == "on" ? "scanned" : "ignored"));
    if (!tui_.cfg_.save_settings(tui_.session_controller_->settings_path()))
        tui_.append_line(P_STATUS,
                         "warning: could not save to " + tui_.session_controller_->settings_path());
    tui_.draw();
}

void SlashDispatcher::cmd_skills_refresh() {
    if (!tui_.win().agent) {
        tui_.append_line(P_STATUS, "no agent in this window");
        return;
    }
    tui_.win().agent->skills().refresh();
    tui_.append_line(P_STATUS, "skills refreshed");
    tui_.draw();
}

void SlashDispatcher::cmd_skills_create(const std::string& args) {
    if (!tui_.win().agent) {
        tui_.append_line(P_STATUS, "no agent in this window");
        return;
    }
    auto trim = [](const std::string& s) {
        size_t b = s.find_first_not_of(" \t");
        size_t e = s.find_last_not_of(" \t");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    std::string name = trim(args);
    std::string scope = "project";
    if (name.rfind("--global", 0) == 0) {
        scope = "global";
        name.clear();
    } else if (name.find(" --global") != std::string::npos) {
        scope = "global";
        name = trim(name.substr(0, name.find(" --global")));
    }
    if (name.empty()) {
        tui_.append_line(P_STATUS, "usage: /set skills create <name> [--global]");
        return;
    }
    std::string err = agent::skill_create(tui_.win().agent->skills(), name, name,
                                          "## " + name + "\n\n(instructions)", scope);
    tui_.append_line(P_STATUS,
                     err.empty() ? ("skill '" + name + "' created (" + scope + ")") : err);
    tui_.draw();
}

void SlashDispatcher::cmd_skills_delete(const std::string& args) {
    if (!tui_.win().agent) {
        tui_.append_line(P_STATUS, "no agent in this window");
        return;
    }
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
        tui_.append_line(P_STATUS, "usage: /set skills delete <name> [--global]");
        return;
    }
    std::string err = agent::skill_delete(tui_.win().agent->skills(), name, scope);
    tui_.append_line(P_STATUS,
                     err.empty() ? ("skill '" + name + "' deleted (" + scope + ")") : err);
    tui_.draw();
}

void SlashDispatcher::cmd_skills_install(const std::string& source) {
    if (source.empty()) {
        tui_.append_line(P_STATUS, "usage: /set skills install <path|url>");
        return;
    }
    std::string global = agent::default_scan_paths().global;
    std::string err = agent::install_skill_pack(source, global);
    if (!err.empty()) {
        tui_.append_line(P_STATUS, "install failed: " + err);
        return;
    }
    if (tui_.win().agent)
        tui_.win().agent->skills().refresh();
    tui_.append_line(P_STATUS, "skill installed to " + global);
    tui_.draw();
}

void SlashDispatcher::cmd_skills_uninstall(const std::string& name) {
    if (name.empty()) {
        tui_.append_line(P_STATUS, "usage: /set skills uninstall <name>");
        return;
    }
    std::string global = agent::default_scan_paths().global;
    std::string err = agent::uninstall_skill(name, global);
    if (!err.empty()) {
        tui_.append_line(P_STATUS, "uninstall failed: " + err);
        return;
    }
    if (tui_.win().agent)
        tui_.win().agent->skills().refresh();
    tui_.append_line(P_STATUS, "skill removed: " + name);
    tui_.draw();
}

void SlashDispatcher::cmd_skills_get(const std::string& sub) {
    std::string name = sub;
    if (!name.empty() && name[0] == ' ')
        name = name.substr(1);
    if (name == "settings") {
        for (const auto& line : agent::skill_acquisition_settings_lines(tui_.cfg_))
            tui_.append_line(P_STATUS, line);
        tui_.draw();
        return;
    }
    if (!tui_.win().agent) {
        tui_.append_line(P_STATUS, "no agent in this window");
        tui_.draw();
        return;
    }
    auto lines = agent::skill_show_lines(tui_.win().agent->skills());
    bool any = false;
    for (const auto& l : lines) {
        if (!name.empty() && l.find(name) == std::string::npos)
            continue;
        any = true;
        tui_.append_line(P_STATUS, l);
    }
    if (!any)
        tui_.append_line(P_STATUS,
                         name.empty() ? "(no skills)" : "no skill matching '" + name + "'");
    tui_.draw();
}

namespace {

// The registered ids, for usage pages and settings listings: derived from the
// runtime, never a hardcoded list.
std::string plugin_ids(const agent::PluginRuntime& runtime) {
    std::string ids;
    for (const auto& p : runtime.list()) {
        if (!ids.empty())
            ids += ' ';
        ids += p.id;
    }
    return ids.empty() ? std::string("(none registered)") : ids;
}

} // namespace

void SlashDispatcher::cmd_plugin_state_usage(const std::string& verb) {
    tui_.append_line(P_STATUS, "usage: /set plugin " + verb +
                                   " <id>  [plugins: " + plugin_ids(tui_.plugin_runtime_) + "]");
}

// Plugin settings are a typed host surface, not a command root. Bundled
// plugins register a SettingsCapability; external plugins keep their manifest
// store until they adopt it.
namespace {

bool print_runtime_settings(const agent::SettingsRegistry& settings, const std::string& id,
                            const std::string& key,
                            const std::function<void(const std::string&)>& out) {
    bool any = false;
    bool key_found = false;
    std::string values;
    std::string selected;
    for (const auto& entry : settings.all()) {
        if (entry.owner != id)
            continue;
        any = true;
        if (key.empty())
            values += entry.spec.key + "=" + settings.get(id, entry.spec.key) + " ";
        else if (entry.spec.key == key) {
            key_found = true;
            selected = settings.get(id, key);
        }
    }
    if (!any)
        return false;
    if (key.empty())
        out("settings " + id + ": " + (values.empty() ? "(none)" : values));
    else if (!key_found)
        out("unknown setting: " + id + " " + key);
    else
        out(id + " " + key + " = " + selected);
    return true;
}

bool print_external_settings(const agent::PluginManager& plugins, const std::string& id,
                             const std::string& key,
                             const std::function<void(const std::string&)>& out) {
    const agent::PluginInfo* plugin = plugins.find(id);
    if (!plugin)
        return false;
    if (key.empty()) {
        std::string values;
        for (auto it = plugin->settings.begin(); it != plugin->settings.end(); ++it)
            values += it.key() + "=" + it.value().dump() + " ";
        out("settings " + id + ": " + (values.empty() ? "(none)" : values));
    } else {
        out(id + " " + key + " = " + plugins.get_setting(id, key));
    }
    return true;
}

} // namespace

void SlashDispatcher::cmd_plugin_settings_get(const std::string& args) {
    const std::size_t sp = args.find(' ');
    const std::string id = (sp == std::string::npos) ? args : args.substr(0, sp);
    if (id.empty()) {
        tui_.append_line(P_STATUS, "usage: /get plugin settings <id> [key]  [plugins: " +
                                       plugin_ids(tui_.plugin_runtime_) + "]");
        return;
    }
    const std::string key = (sp == std::string::npos) ? "" : args.substr(sp + 1);
    const auto out = [this](const std::string& line) { tui_.append_line(P_STATUS, line); };
    if (print_runtime_settings(tui_.plugin_runtime_.settings(), id, key, out))
        return;
    if (print_external_settings(tui_.plugins_, id, key, out))
        return;
    tui_.append_line(P_STATUS, "unknown plugin: " + id);
}

void SlashDispatcher::cmd_plugin_settings_set(const std::string& args) {
    std::size_t sp = args.find(' ');
    if (sp == std::string::npos) {
        tui_.append_line(P_STATUS, "usage: /set plugin settings <id> <key> <value>");
        return;
    }
    const std::string id = args.substr(0, sp);
    std::string rest = args.substr(sp + 1);
    std::size_t sp2 = rest.find(' ');
    std::string key;
    std::string value;
    if (sp2 == std::string::npos) {
        const std::size_t eq = rest.find('=');
        if (eq == std::string::npos) {
            tui_.append_line(P_STATUS, "usage: /set plugin settings <id> <key> <value>");
            return;
        }
        key = rest.substr(0, eq);
        value = rest.substr(eq + 1);
    } else {
        key = rest.substr(0, sp2);
        value = rest.substr(sp2 + 1);
    }
    if (tui_.plugin_runtime_.settings().set(id, key, value)) {
        tui_.append_line(P_STATUS, "set " + id + " " + key + " = " + value);
        return;
    }
    if (tui_.plugins_.set_setting(id, key, value))
        tui_.append_line(P_STATUS, "set " + id + " " + key + " = " + value);
    else
        tui_.append_line(P_STATUS, "unknown plugin or setting: " + id + " " + key);
}

// Installing stages the archive and leaves the plugin off: activation is a
// separate, deliberate step.
void SlashDispatcher::cmd_plugin_install(const std::string& source) {
    if (source.empty()) {
        tui_.append_line(P_STATUS, "usage: /set plugin install <path|url>");
        return;
    }
    tui_.append_line(P_STATUS, "installing " + source + " ...");
    std::string err = tui_.plugins_.install(source);
    if (!err.empty()) {
        tui_.append_line(P_STATUS, "install failed: " + err);
        return;
    }
    tui_.plugins_.discover();
    // Hand the staged plugin to the runtime so it appears in the registry and
    // the feed right away; registration is idempotent per id.
    tui_.plugin_runtime_.add_external(tui_.plugins_);
    refresh_completions();
    tui_.append_line(P_STATUS, "installed \u2014 '/set plugin on <id>' activates it");
}

void SlashDispatcher::cmd_plugin_uninstall(const std::string& id) {
    if (id.empty()) {
        tui_.append_line(P_STATUS, "usage: /set plugin uninstall <id>");
        return;
    }
    // Stop a live plugin before its files go away; the registry entry itself
    // is dropped on the next start.
    const bool registered = tui_.plugin_runtime_.has(id);
    if (registered)
        tui_.plugin_runtime_.set_state(id, false);
    std::string err = tui_.plugins_.uninstall(id);
    if (!err.empty()) {
        tui_.append_line(P_STATUS, "uninstall failed: " + err);
        return;
    }
    tui_.plugins_.discover();
    refresh_completions();
    tui_.append_line(P_STATUS, "uninstalled: " + id +
                                   (registered ? " \u2014 registry entry clears on restart" : ""));
}

const std::vector<palette::Command>& SlashDispatcher::commands() {
    if (commands_.empty())
        build_commands();
    return commands_;
}

void SlashDispatcher::build_commands() {
    commands_.clear();
    // Display-only command list, generated from the JSON tree — the tree
    // is the single source of truth; nothing here is hardcoded.
    for (const auto& name : tui_.settings_.complete("")) {
        palette::Command c;
        c.name = name;
        c.help = tui_.settings_.help_for(name);
        c.aliases = tui_.settings_.aliases_for(name);
        c.short_name = tui_.settings_.short_for(name);
        const auto kids = tui_.settings_.children_of(name);
        if (!kids.empty()) {
            std::string args;
            for (size_t i = 0; i < kids.size(); ++i) {
                if (i > 0)
                    args += '|';
                args += kids[i];
            }
            if (args.size() > 40)
                args = "option";
            c.args = "<" + args + ">";
        }
        commands_.push_back(std::move(c));
    }
    register_builtin_actions();
}

void SlashDispatcher::register_action(const std::string& action,
                                      std::function<void(const std::string&)> handler) {
    action_registry_.register_action(action, std::move(handler));
}

void SlashDispatcher::register_builtin_actions() {
    register_core_actions();
    register_provider_actions();
    register_os_actions();
    register_config_set_actions();
    register_config_get_actions();
}

// /window*: screen lifecycle and resource attachment.
void SlashDispatcher::register_window_actions() {
    register_action("core.window.close", [this](const std::string&) { tui_.close_window(); });
    register_action("core.window.new", [this](const std::string& a) { cmd_window_new(false, a); });
    register_action("core.window.new.noworkdir",
                    [this](const std::string& a) { cmd_window_new(true, a); });
    register_action("core.window.list", [this](const std::string&) { cmd_window_list(); });
    register_action("core.window.rename", [this](const std::string& a) { cmd_window_rename(a); });
    register_action("core.window.set", [this](const std::string& a) { cmd_window_set(a); });
    register_action("core.window.attach", [this](const std::string& a) {
        const std::size_t sp = a.find(' ');
        if (sp == std::string::npos) {
            tui_.append_line(P_STATUS, "usage: /window attach <job|agent|watch> <id>");
            return;
        }
        open_resource(a.substr(0, sp), a.substr(sp + 1));
    });
    register_action("core.window.detach", [this](const std::string&) {
        if (tui_.win().kind != Window::Kind::Resource) {
            tui_.append_line(P_STATUS, "detach: active window is not a resource viewer");
            return;
        }
        tui_.close_window();
    });
    register_action("core.window", [this](const std::string& a) {
        if (!a.empty())
            tui_.append_line(P_STATUS,
                             "usage: /window new [name] | new noworkdir [name] | close | list | "
                             "rename | set <number> | attach <kind> <id> | detach");
        cmd_window_list();
    });
}

// /help, /settings, /prompt, /session reset, /window*, /stop.
void SlashDispatcher::register_core_session_actions() {
    register_action("core.help", [this](const std::string& a) { cmd_help(a); });
    register_action("core.settings", [this](const std::string&) { cmd_settings(); });
    register_action("core.settings.provider", [this](const std::string&) {
        tui_.settings_screen();
        tui_.redraw_after_modal();
    });
    register_action("core.session.reset", [this](const std::string&) { cmd_session_reset(); });
    register_window_actions();
    register_action("core.stop", [this](const std::string&) {
        // /stop cancels the ACTIVE window's run only.
        if (tui_.win().agent)
            tui_.win().agent->request_cancel();
        tui_.runs_.request_cancel(tui_.win().id);
        tui_.append_line(P_STATUS, "stop requested");
    });
}

// /compress, /job*, /session*, /quit.
void SlashDispatcher::register_core_job_actions() {
    register_action("core.compress", [this](const std::string&) { cmd_compress(""); });
    register_action("core.job", [this](const std::string& a) { cmd_job(a); });
    register_action("core.job.list", [this](const std::string&) { job_ls(); });
    register_action("core.job.stop", [this](const std::string& a) { job_kill(a); });
    register_action("core.job.read", [this](const std::string& a) { job_read(a); });
    register_action("core.job.start", [this](const std::string& a) { job_start(a); });
    register_action("core.session.save",
                    [this](const std::string&) { tui_.session_controller_->save_session(); });
    register_action("core.session.list",
                    [this](const std::string&) { tui_.session_controller_->session_browser(); });
    register_action("core.session.load", [this](const std::string& a) { cmd_session_load(a); });
    register_action("core.session.remove", [this](const std::string& a) { cmd_session_delete(a); });
    register_action("core.session.rename", [this](const std::string& a) { cmd_session_rename(a); });
    register_action("core.session.fork",
                    [this](const std::string&) { tui_.session_controller_->fork_session(); });
    register_action("core.session", [this](const std::string& a) {
        if (!a.empty())
            tui_.append_line(P_STATUS,
                             "usage: /session new|list|save|load|remove|rename|fork|stop");
        tui_.session_controller_->session_browser();
    });
    register_action("core.quit", [this](const std::string&) { request_quit(); });
}

void SlashDispatcher::register_core_actions() {
    register_core_session_actions();
    register_core_job_actions();
    register_worktree_actions();
}

// /worktree: the static verbs. The attach/remove name leaves are contributed by
// FeedManager::refresh_worktree_feed(), each with a host-derived action.
void SlashDispatcher::register_worktree_actions() {
    register_action("core.worktree", [this](const std::string& a) { cmd_worktree(a); });
    register_action("core.worktree.new", [this](const std::string& a) { cmd_worktree_new(a); });
    register_action("core.worktree.attach",
                    [this](const std::string& a) { cmd_worktree_attach(a); });
    register_action("core.worktree.detach", [this](const std::string&) { cmd_worktree_detach(); });
    register_action("core.worktree.remove",
                    [this](const std::string& a) { cmd_worktree_remove(a); });
    register_action("core.worktree.status", [this](const std::string&) { cmd_worktree_status(); });
    register_action("core.worktree.list", [this](const std::string&) { cmd_worktree_list(); });
    register_action("core.worktree.prune", [this](const std::string&) { cmd_worktree_prune(); });
    register_action("core.worktree.main", [this](const std::string&) { cmd_worktree_main(); });
}

void SlashDispatcher::register_provider_actions() {
    // provider
    register_action("core.provider", [this](const std::string& a) { cmd_provider(a); });
    register_action("core.provider.list", [this](const std::string&) { cmd_provider_list(); });
    register_action("core.provider.delete",
                    [this](const std::string& a) { cmd_provider_delete(a); });
    register_action("core.provider.test", [this](const std::string& a) { cmd_provider_test(a); });
    // model (get/set accessor — see completions.json get.model/set.model)
}

void SlashDispatcher::register_os_actions() {
    // files
    register_action("os.files", [this](const std::string& a) {
        if (!a.empty())
            tui_.append_line(P_STATUS, "usage: /files ls|tree|open|find <path>");
        cmd_files_ls("");
    });
    register_action("os.files.ls", [this](const std::string& a) { cmd_files_ls(a); });
    register_action("os.files.tree", [this](const std::string& a) { cmd_files_tree(a); });
    register_action("os.files.open", [this](const std::string& a) { cmd_files_open(a); });
    register_action("os.files.find", [this](const std::string& a) { cmd_files_find(a); });
    // system
    register_action("os.system", [this](const std::string& a) {
        if (!a.empty())
            tui_.append_line(
                P_STATUS,
                "usage: /system exec|delete|rmdir|mkdir|mv|cp|info|ps|kill|df|uptime|uname");
        tui_.append_line(P_STATUS, "system operations: /system exec <cmd> | ps | df | uptime | "
                                   "uname | kill <pid> | info <path>");
    });
    register_action("os.system.exec", [this](const std::string& a) { cmd_system_exec(a); });
    register_action("os.system.delete", [this](const std::string& a) { cmd_system_delete(a); });
    register_action("os.system.rmdir", [this](const std::string& a) { cmd_system_rmdir(a); });
    register_action("os.system.mkdir", [this](const std::string& a) { cmd_system_mkdir(a); });
    register_action("os.system.mv", [this](const std::string& a) { cmd_system_mv(a); });
    register_action("os.system.cp", [this](const std::string& a) { cmd_system_cp(a); });
    register_action("os.system.info", [this](const std::string& a) { cmd_system_info(a); });
    register_action("os.system.ps", [this](const std::string&) { cmd_system_ps(); });
    register_action("os.system.kill", [this](const std::string& a) { cmd_system_kill(a); });
    register_action("os.system.df", [this](const std::string&) { cmd_system_df(); });
    register_action("os.system.uptime", [this](const std::string&) { cmd_system_uptime(); });
    register_action("os.system.uname", [this](const std::string&) { cmd_system_uname(); });
}

// /set detection, subagent, display and policy.
void SlashDispatcher::register_set_engine_actions() {
    register_action("core.config.set", [this](const std::string& a) { cmd_set(a); });
    register_action("core.config.set.detection.loop",
                    [this](const std::string& v) { cmd_set_detection_toggle("loop", v); });
    register_action("core.config.set.worktree",
                    [this](const std::string& v) { cmd_set_worktree(v); });
    // Routed through the generic settings path so the one save site (and its
    // documented behaviour) is shared with every other dotted setting.
    register_action("core.config.set.work.auto_attach", [this](const std::string& v) {
        if (!apply_setting_key("work.auto_attach " + v))
            tui_.append_line(P_STATUS, "usage: /set work auto_attach off|agents|jobs|all");
    });
    register_action("core.config.set.detection.duplicate",
                    [this](const std::string& v) { cmd_set_detection_toggle("duplicate", v); });
    register_action("core.config.set.debug", [this](const std::string& v) { set_debug(v); });
    register_action("core.config.set.subagent.parallel",
                    [this](const std::string& v) { cmd_set_subagent_parallel(v); });
    register_action("core.config.set.subagent.max",
                    [this](const std::string& v) { cmd_set_subagent_max(v); });
    register_action("core.config.set.display.markdown", [this](const std::string& v) {
        if (v != "on" && v != "off") {
            tui_.append_line(P_STATUS, "usage: /set display markdown on|off");
            return;
        }
        tui_.win().markdown_on = (v == "on");
        tui_.append_line(P_STATUS, "markdown rendering: " + v);
        tui_.draw();
    });
}

// /set policy.
void SlashDispatcher::register_set_policy_actions() {
    register_action("core.config.set.policy.mode", [this](const std::string& v) {
        if (v != "read" && v != "write" && v != "yolo") {
            tui_.append_line(P_STATUS, "usage: /set policy mode read|write|yolo");
            return;
        }
        // One path for the command and the dotted setting: the active window's
        // agent adopts the mode (its approval gate reads it), the template gets
        // it for new windows, and set_mode publishes the snapshot the bar reads.
        apply_policy_mode(v);
        tui_.append_line(P_STATUS, "policy mode: " + v);
        tui_.draw();
    });
    register_action("core.config.set.policy.approval", [this](const std::string& v) {
        if (v != "on" && v != "off") {
            tui_.append_line(P_STATUS, "usage: /set policy approval on|off");
            return;
        }
        tui_.cfg_.policy_approval = (v == "on");
        tui_.append_line(P_STATUS, "policy approval: " + v);
    });
    register_action("core.config.set.policy.timeout", [this](const std::string& v) {
        auto n = text::parse_setting_int(v, 0, 999);
        if (!n) {
            tui_.append_line(P_STATUS, "policy.timeout: invalid value (0-999): " + v);
            return;
        }
        tui_.policy_timeout_ = *n;
        tui_.append_line(P_STATUS, "policy timeout: " + std::to_string(*n) + "s");
    });
    register_action("core.config.set.policy",
                    [this](const std::string&) { cmd_set_policy_usage(); });
    register_action("core.config.set.provider", [this](const std::string& a) { cmd_provider(a); });
    register_action("core.config.set.policy.rule",
                    [this](const std::string& a) { cmd_set_policy_rule(a); });
}

// /set compression, think and skills.
// The acquisition settings are host-level, so they register as one group next
// to the explorer verbs rather than inside the feature-settings table.
void wire_skill_setting_actions(tui::ActionRegistry& registry,
                                const std::function<void(const std::string&)>& set_skills,
                                const std::function<void(const std::string&)>& get_settings) {
    for (const char* key : {"review", "audit", "update-review", "update-check", "scope", "index"}) {
        std::string name = "core.config.set.skills.";
        name.append(key);
        const std::string verb = key;
        registry.register_action(name, [set_skills, verb](const std::string& v) {
            std::string args = verb;
            args.append(" ").append(v);
            set_skills(args);
        });
    }
    registry.register_action("core.config.get.skills.settings",
                             [get_settings](const std::string&) { get_settings("settings"); });
}

void SlashDispatcher::register_skills_actions() {
    auto run = [this](const tui::SkillsExplorerOptions& opts) {
        tui::run_skills_explorer(
            opts, [this](const std::string& line) { tui_.append_line(P_STATUS, line); });
        tui_.redraw_after_modal();
    };
    auto options = [this](const std::string& arg) {
        tui::SkillsExplorerOptions opts;
        tui_.configure_skills_explorer(opts, arg);
        return opts;
    };
    auto ready = [this] {
        if (!tui_.win().agent) {
            tui_.append_line(P_STATUS, "no agent in this window");
            return false;
        }
        tui_.win().agent->skills().refresh();
        return true;
    };
    wire_skills_explorer_verbs(action_registry_, run, options, ready);
    wire_skills_readouts(
        action_registry_, [this] { return agent::skill_show_lines(tui_.win().agent->skills()); },
        [this](const std::string& line) { tui_.append_line(P_STATUS, line); },
        [this](const std::string& a) { cmd_skills_get(a); },
        [this] {
            for (const auto& line : skill_sources_lines())
                tui_.append_line(P_STATUS, line);
        });
    wire_skill_setting_actions(
        action_registry_, [this](const std::string& a) { cmd_skills_set(a); },
        [this](const std::string& a) { cmd_skills_get(a); });
}

void SlashDispatcher::register_set_feature_actions() {
    register_action("core.config.set.compression.threshold",
                    [this](const std::string& v) { apply_compression_threshold(v); });
    register_action("core.config.set.compression.min_turns",
                    [this](const std::string& v) { apply_compression_min_turns(v); });
    register_action("core.config.set.compression.target_pct",
                    [this](const std::string& v) { apply_compression_target_pct(v); });
    register_action("core.config.set.compression.keep_last_prompts",
                    [this](const std::string& v) { apply_compression_keep_last_prompts(v); });
    register_action("core.config.set.compression", [this](const std::string& a) { cmd_set(a); });
    register_action("core.config.set.think", [this](const std::string& v) {
        if (v != "on" && v != "off" && v != "auto") {
            tui_.append_line(P_STATUS, "usage: /set think on|off|auto");
            return;
        }
        tui_.cfg_.thinking = v;
        if (tui_.win().agent)
            tui_.win().agent->set_thinking(v);
        tui_.append_line(P_STATUS, "thinking: " + v);
    });
    register_skills_actions();
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
}

void SlashDispatcher::register_config_set_actions() {
    register_set_engine_actions();
    register_set_policy_actions();
    register_set_feature_actions();
}

// /get config, model, mcp, learn and the panel.
void SlashDispatcher::register_get_config_actions() {
    register_action("core.config.get", [this](const std::string& a) { cmd_get(a); });
    register_action("core.config.get.config", [this](const std::string&) { cmd_get_config(); });
    register_action("core.config.get.config.paths",
                    [this](const std::string&) { cmd_get_config_paths(); });
    register_action("core.config.get.model", [this](const std::string&) { cmd_get_model(); });
    register_action("core.config.get.model.list",
                    [this](const std::string&) { cmd_get_model_list(); });
    register_action("core.config.get.model.context",
                    [this](const std::string&) { cmd_get_model_context(); });
    register_action("core.config.set.model", [this](const std::string& a) { cmd_model_set(a); });
    register_action("core.config.set.model.context",
                    [this](const std::string& a) { cmd_model_context(a); });
    register_action("core.config.get.learn", [this](const std::string& a) { cmd_get(a); });
    register_action("core.panel", [this](const std::string& a) { tui_.open_panels(a); });
}

// /get plugin, /set plugin, /get provider, /get policy, display, think,
// detection and subagent.
void SlashDispatcher::register_get_plugin_policy_actions() {
    register_plugin_actions();
    register_provider_wallet_actions();
    register_policy_actions();
    register_readout_actions();
}

// /get plugin.* and /set plugin.*
void SlashDispatcher::register_plugin_actions() {
    register_action("core.config.get.plugin",
                    [this](const std::string& a) { cmd_runtime_plugin_get(a); });
    register_action("core.config.get.plugin.list",
                    [this](const std::string&) { cmd_runtime_plugin_list(); });
    register_action("core.config.get.plugin.info",
                    [this](const std::string& a) { cmd_runtime_plugin_get(a); });
    register_action("core.config.get.plugin.settings",
                    [this](const std::string& a) { cmd_plugin_settings_get(a); });
    register_action("core.config.set.plugin.on",
                    [this](const std::string&) { cmd_plugin_state_usage("on"); });
    register_action("core.config.set.plugin.off",
                    [this](const std::string&) { cmd_plugin_state_usage("off"); });
    register_action("core.config.set.plugin.install",
                    [this](const std::string& a) { cmd_plugin_install(a); });
    register_action("core.config.set.plugin.uninstall",
                    [this](const std::string& a) { cmd_plugin_uninstall(a); });
    register_action("core.config.set.plugin.settings",
                    [this](const std::string& a) { cmd_plugin_settings_set(a); });
    register_action("core.work", [this](const std::string&) { cmd_work_board(); });
    register_action("core.usage.show", [this](const std::string&) { cmd_usage_show(); });
    register_action("core.usage.reset", [this](const std::string&) { cmd_usage_reset(); });
    register_action("core.project", [this](const std::string&) { cmd_project_list(); });
    register_action("core.project.list", [this](const std::string&) { cmd_project_list(); });
    register_action("core.project.open", [this](const std::string& a) { cmd_project_open(a); });
    register_action("core.project.show", [this](const std::string&) { cmd_project_show(); });
    register_action("core.project.close", [this](const std::string&) { cmd_project_close(); });
    register_action("core.plugin", [this](const std::string& a) {
        if (!a.empty())
            tui_.append_line(P_STATUS, "usage: /plugin <id> <command>  [see /get plugin list]");
        cmd_runtime_plugin_list();
    });
}

// /get provider*, and the wallet pair under provider.
void SlashDispatcher::register_provider_wallet_actions() {
    register_action("core.config.get.provider.wallet",
                    [this](const std::string&) { cmd_get_wallet(); });
    register_action("core.config.set.provider.wallet",
                    [this](const std::string& a) { cmd_set_wallet(a); });
    register_action("core.config.get.provider", [this](const std::string&) { cmd_get_provider(); });
    register_action("core.config.get.provider.list",
                    [this](const std::string&) { cmd_provider_list(); });
}

// /get policy*
void SlashDispatcher::register_policy_actions() {
    register_action("core.config.get.policy", [this](const std::string& a) { cmd_get_policy(a); });
    register_action("core.config.get.policy.mode",
                    [this](const std::string&) { cmd_get_policy_mode(); });
    register_action("core.config.get.policy.approval",
                    [this](const std::string&) { cmd_get_policy_approval(); });
    register_action("core.config.get.policy.timeout",
                    [this](const std::string&) { cmd_get_policy_timeout(); });
    register_action("core.config.get.policy.rule",
                    [this](const std::string& a) { cmd_get_policy_rule(a); });
}

// The remaining /get readouts: display, think, detection, subagent.
void SlashDispatcher::register_readout_actions() {
    register_action("core.config.get.display", [this](const std::string&) { cmd_get_display(); });
    register_action("core.config.get.think", [this](const std::string&) { cmd_get_think(); });
    register_action("core.config.get.detection",
                    [this](const std::string& a) { cmd_get_detection(a); });
    register_action("core.config.get.detection.loop",
                    [this](const std::string&) { cmd_get_detection("loop"); });
    register_action("core.config.get.worktree", [this](const std::string&) { cmd_get_worktree(); });
    register_action("core.config.get.work", [this](const std::string&) { cmd_get("work"); });
    register_action("core.config.get.detection.duplicate",
                    [this](const std::string&) { cmd_get_detection("duplicate"); });
    register_action("core.config.get.debug", [this](const std::string&) { show_debug_status(); });
    register_action("core.config.get.subagent", [this](const std::string&) { cmd_get_subagent(); });
}

// /set reasoning, /get reasoning, /get compression and /get skills.
void SlashDispatcher::register_get_reasoning_actions() {
    register_action("core.config.set.reasoning.effort",
                    [this](const std::string& v) { cmd_set_reasoning_effort(v); });
    // Bare /set reasoning <val>: same handler as the effort leaf so
    // "/set reasoning high" works, not just the fully-qualified path.
    register_action("core.config.set.reasoning", [this](const std::string& v) {
        if (v == "off" || v == "low" || v == "medium" || v == "high")
            cmd_set_reasoning_effort(v);
        else
            tui_.append_line(P_STATUS, "usage: /set reasoning <off|low|medium|high>");
    });
    register_action("core.config.get.reasoning",
                    [this](const std::string&) { cmd_get_reasoning(); });
    register_action("core.config.get.compression",
                    [this](const std::string&) { cmd_get_compression(); });
    register_action("core.config.get.compression.threshold", [this](const std::string&) {
        tui_.append_line(P_STATUS, "compression threshold: " +
                                       std::to_string(compression_threshold_effective()));
    });
    register_action("core.config.get.compression.min_turns", [this](const std::string&) {
        tui_.append_line(P_STATUS,
                         "compression min_turns: " +
                             std::to_string(agent::load_compression_config(tui_.cfg_).min_turns));
    });
    register_action("core.config.get.compression.target_pct", [this](const std::string&) {
        tui_.append_line(P_STATUS,
                         "compression target_pct: " +
                             std::to_string(agent::load_compression_config(tui_.cfg_).target_pct));
    });
    register_action("core.config.get.compression.keep_last_prompts", [this](const std::string&) {
        tui_.append_line(
            P_STATUS,
            "compression keep_last_prompts: " +
                std::to_string(agent::load_compression_config(tui_.cfg_).keep_last_prompts));
    });
    register_action("core.config.get.skills", [this](const std::string& a) { cmd_skills_get(a); });
    // The leaf lists everything; passing "show" here made it a NAME FILTER, so
    // the documented "/get skills show" matched nothing.
    register_action("core.config.get.skills.show",
                    [this](const std::string&) { cmd_skills_get(""); });
}

void SlashDispatcher::register_config_get_actions() {
    register_get_config_actions();
    register_get_plugin_policy_actions();
    register_get_reasoning_actions();
}

const palette::Command* SlashDispatcher::find_command(const std::string& name) {
    return palette::find(commands(), name);
}

// Trailing whitespace off the command line.
std::string trim_slash_line(const std::string& line) {
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();
    return trimmed;
}

std::vector<std::string> tokenize_slash(const std::string& text) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : text) {
        if (c != ' ' && c != '\t') {
            cur += c;
            continue;
        }
        if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        tokens.push_back(cur);
    return tokens;
}

// The unclaimed tokens, joined back into the handler's argument string.
std::string join_tokens(const std::vector<std::string>& tokens, size_t from) {
    std::string arg;
    for (size_t i = from; i < tokens.size(); ++i) {
        if (!arg.empty())
            arg += ' ';
        arg += tokens[i];
    }
    return arg;
}

// Documented in the tree but no handler: show the node's manual page.
void SlashDispatcher::report_missing_handler(const json* node, const std::string& first) {
    if (node->contains("man") && (*node)["man"].is_string())
        tui_.append_line(P_STATUS, "/" + first + ": " + (*node)["man"].get<std::string>());
    else
        tui_.append_line(P_STATUS, "/" + first + ": no handler for this action");
}

bool SlashDispatcher::handle_slash(const std::string& line) {
    if (line.empty() || line[0] != '/')
        return false;
    const std::vector<std::string> tokens = tokenize_slash(trim_slash_line(line).substr(1));
    if (tokens.empty())
        return true;

    const tui::SettingRegistry::CommandMatch match = tui_.settings_.resolve_command(tokens);
    if (match.ambiguous) {
        std::string candidates;
        for (const auto& c : match.candidates) {
            if (!candidates.empty())
                candidates += ", ";
            candidates += "/" + c;
        }
        tui_.append_line(P_STATUS, "ambiguous command: /" + tokens[0] + " matches " + candidates +
                                       "  (type more)");
        return true;
    }
    if (!match.node) {
        tui_.append_line(P_STATUS, "unknown command: /" + tokens[0] + "  (try /help)");
        return true;
    }

    const std::string action =
        (match.node->contains("action") && (*match.node)["action"].is_string())
            ? (*match.node)["action"].get<std::string>()
            : std::string();
    if (!action_registry_.has(action)) {
        report_missing_handler(match.node, tokens[0]);
        return true;
    }
    try {
        action_registry_.dispatch(action, join_tokens(tokens, match.consumed));
    } catch (const std::exception& e) {
        // No slash command may crash the TUI (uncaught exceptions from setters
        // used to abort() with the terminal left in raw mode).
        tui_.append_line(P_STATUS, std::string("command error: ") + e.what());
    }
    return true;
}

std::string SlashDispatcher::usage(const palette::Command& c) const {
    return palette::usage(c);
}

void SlashDispatcher::cmd_model_set(const std::string& arg) {
    if (busy_reject("model"))
        return;
    if (arg.empty()) {
        tui_.append_line(P_STATUS, "model: " + tui_.cfg_.model +
                                       " \u2014 /model <name> or /set model to switch");
        return;
    }
    // Validate against the cached catalog — never a live fetch on the UI
    // thread. A cold catalog kicks the background refresh and asks the user
    // to retry once it lands.
    auto models = agent::list_model_info_cached(tui_.cfg_);
    auto entry = agent::model_catalog_read(tui_.cfg_);
    if (!entry || !agent::model_catalog_fresh(*entry))
        tui_.refresh_models_async(false);
    if (models.empty() && !entry) {
        tui_.append_line(P_STATUS, "model list not loaded yet - retry in a moment");
        return;
    }
    const ModelChoice choice = find_model(models, arg);
    if (!choice.found) {
        tui_.append_line(P_STATUS, "model \"" + arg + "\" not found in provider's model list");
        return;
    }
    // Model is per-window: apply to the ACTIVE window's agent. cfg_ stays
    // the template for future windows; siblings keep their own model.
    tui_.cfg_.model = arg;
    tui_.cfg_.model_explicit = true;
    agent::apply_model_capabilities(tui_.cfg_, choice.caps);
    if (tui_.win().agent)
        tui_.win().agent->set_model(arg, choice.caps);
    std::string global = agent::global_config_path();
    tui_.cfg_.save_global(global);
    tui_.providers_->remember_model(tui_.cfg_.provider_name, arg);
    tui_.append_line(P_STATUS,
                     "model set to " + arg + " (remembered for " + tui_.cfg_.provider_name + ")");
}

void SlashDispatcher::show_model_context() {
    const auto acfg = tui_.win().agent ? tui_.win().agent->config_snapshot() : nullptr;
    const int ctx = acfg ? acfg->context_size : tui_.cfg_.context_size;
    const bool pinned = acfg ? acfg->context_explicit : tui_.cfg_.context_explicit;
    std::string line = "context window: ";
    if (pinned && ctx > 0)
        line += std::to_string(ctx) + " tokens (pinned)";
    else if (ctx > 0)
        line += "auto (" + std::to_string(ctx) + " tokens)";
    else
        line += "auto (unknown)";
    tui_.append_line(P_STATUS, line);
}

void SlashDispatcher::clear_model_context() {
    tui_.cfg_.context_explicit = false;
    tui_.cfg_.context_size = 0;
    if (tui_.win().agent) {
        tui_.win().agent->clear_context_pin();
        const auto now = tui_.win().agent->config_snapshot();
        if (now)
            tui_.cfg_.context_size = now->context_size;
    }
    // The writer omits context_size when it is not explicit, so clearing
    // removes the file pin as well as the live one.
    tui_.cfg_.save_global(agent::global_config_path());
    std::string msg = "context window: auto";
    if (tui_.cfg_.context_size > 0)
        msg += " (" + std::to_string(tui_.cfg_.context_size) + " tokens)";
    tui_.append_line(P_STATUS, msg);
    tui_.draw();
}

void SlashDispatcher::pin_model_context(int window) {
    tui_.cfg_.context_size = window;
    tui_.cfg_.context_explicit = true;
    if (tui_.win().agent)
        tui_.win().agent->pin_context_window(window);
    tui_.cfg_.save_global(agent::global_config_path());
    tui_.append_line(P_STATUS, "context window pinned: " + std::to_string(window) + " tokens");
    tui_.draw();
}

void SlashDispatcher::cmd_model_context(const std::string& arg) {
    const std::string& v = arg;
    if (v.empty()) {
        show_model_context();
        return;
    }
    if (v == "0" || v == "auto") {
        clear_model_context();
        return;
    }
    auto n = text::parse_setting_int(v, 1024, 10000000);
    if (!n) {
        tui_.append_line(P_STATUS, "usage: /set model context <tokens >= 1024> | 0 | auto");
        return;
    }
    pin_model_context(*n);
}

void SlashDispatcher::cmd_model_set_for(const std::string& provider, const std::string& id) {
    if (busy_reject("model"))
        return;
    if (provider == tui_.cfg_.provider_name) {
        cmd_model_set(id);
        return;
    }
    // Switch first: cmd_model_set() validates against the ACTIVE provider's
    // cached catalogue, so validating before the switch would reject a model
    // that is perfectly valid for the provider the user just picked.
    auto sel = tui_.providers_->select(provider);
    // The same landing /provider gives, key prompt included: the drawer named
    // the provider, so a missing key is one form away -- not a warning the user
    // has to translate into a second command. Cancelling the form returns with
    // nothing changed.
    if (!ensure_provider_ready(provider, sel))
        return;
    agent::apply_selection(tui_.cfg_, sel);
    if (!sel.warning.empty())
        tui_.append_line(P_STATUS, "provider " + provider + ": " + sel.warning);
    // The switch has to reach the running app, not just the host template: the
    // window agent's endpoint and the bar's provider both come from IT, and on
    // the old wiring a kilo model was requested from openrouter while the bar
    // said openrouter.
    land_provider_switch();
    cmd_model_set(id);
}

void SlashDispatcher::cmd_get_model() {
    tui_.append_line(P_STATUS,
                     "model: " + tui_.cfg_.model + " (provider: " + tui_.cfg_.provider_name + ")");
}

void SlashDispatcher::cmd_get_model_list() {
    refresh_model_list();
    auto entry = agent::model_catalog_read(tui_.cfg_);
    if (!entry || !agent::model_catalog_fresh(*entry)) {
        // The refresh may fail, and the user who asked must hear why -- the
        // failure arrives after this line, through on_models_refreshed.
        tui_.report_model_failure_ = true;
        tui_.refresh_models_async(false);
    }
    if (model_info_.empty()) {
        tui_.append_line(P_STATUS, "no cached model list yet - refreshing ...");
        return;
    }
    for (const auto& m : model_info_) {
        const int ctx = m.context ? m.context : m.context_train;
        std::string line = "  " + m.id;
        if (ctx > 0)
            line += "  in " + compact_tokens(ctx);
        if (m.max_output > 0)
            line += " | out " + compact_tokens(m.max_output);
        if (m.thinking_adaptive)
            line +=
                " | thinking adaptive" + (m.default_effort.empty() ? "" : " " + m.default_effort);
        else if (m.thinking_legacy)
            line += " | thinking budget";
        else if (m.reasoning)
            line += " | reasoning";
        if (!m.input_modalities.empty()) {
            line += " | in";
            for (const auto& modality : m.input_modalities)
                line += ":" + modality;
        }
        tui_.append_line(P_ASSISTANT, line);
    }
}

void SlashDispatcher::cmd_get_model_context() {
    agent::ServerInfo info = agent::probe_server_cached(tui_.cfg_);
    auto entry = agent::model_catalog_read(tui_.cfg_);
    if (!entry || !agent::model_catalog_fresh(*entry))
        tui_.refresh_models_async(false);
    std::string line = "model: " + tui_.cfg_.model;
    const auto acfg = tui_.win().agent ? tui_.win().agent->config_snapshot() : nullptr;
    if (info.ok && info.context_size > 0) {
        line += "  ctx: " + std::to_string(info.context_size);
        if (info.context_train > 0 && info.context_train != info.context_size)
            line += " (max " + std::to_string(info.context_train) + ")";
    } else if (const int ctx = acfg ? acfg->context_size : tui_.cfg_.context_size; ctx > 0) {
        line += "  ctx: " + std::to_string(ctx) + " (cached)";
    } else {
        line += "  ctx: unknown";
    }
    tui_.append_line(P_STATUS, line);
}

void SlashDispatcher::refresh_settings_feed() {
    // /settings is a namespace fed from the same tree as /set: the group list,
    // help and man are the JSON mapper's, not a code-side list. The generated
    // actions dispatch the matching /get readout; the provider child opens the
    // provider picker. Editing stays /set, so no setting logic is duplicated.
    nlohmann::json sub;
    auto& kids = sub["settings"]["children"];
    for (const auto& g : tui_.settings_.children_of("set")) {
        if (g == "provider")
            continue;
        const std::string key = "set." + g;
        kids[g]["action"] = "core.settings.group." + g;
        const std::string help = tui_.settings_.help_for(key);
        if (!help.empty())
            kids[g]["help"] = help;
        const std::string man = tui_.settings_.man_for(key);
        if (!man.empty())
            kids[g]["man"] = man;
        register_action("core.settings.group." + g,
                        [this, g](const std::string&) { cmd_settings_group(g); });
    }
    kids["provider"]["action"] = "core.settings.provider";
    kids["provider"]["help"] = "Manage API providers: add, edit, key and connection test.";
    kids["provider"]["man"] =
        "Opens the provider picker. Selecting a provider offers activate/edit, key entry and a "
        "connection test; changes are persisted under ~/.config/amber/.";
    tui_.settings_.merge_completions_json(sub);
}

void SlashDispatcher::cmd_settings() {
    tui_.append_line(P_STATUS, "settings groups (edit with /set <group>):");
    for (const auto& g : tui_.settings_.children_of("settings")) {
        std::string line = "  " + g;
        const std::string help = tui_.settings_.help_for("settings." + g);
        if (!help.empty())
            line += "  \u2014  " + help;
        tui_.append_line(P_STATUS, line);
    }
    tui_.draw();
}

void SlashDispatcher::cmd_settings_group(const std::string& group) {
    const auto& tree = tui_.settings_.command_tree();
    const auto& get = tree["commands"]["get"]["children"];
    if (get.contains(group)) {
        const auto& node = get[group];
        const std::string action = (node.contains("action") && node["action"].is_string())
                                       ? node["action"].get<std::string>()
                                       : std::string();
        if (!action.empty() && action_registry_.has(action)) {
            action_registry_.dispatch(action, "");
            return;
        }
    }
    tui_.append_line(P_STATUS, "settings " + group + " \u2014 use /set " + group);
    tui_.draw();
}

void SlashDispatcher::refresh_model_list() {
    if (tui_.feed_manager_)
        tui_.feed_manager_->refresh_model_list();
}

// First-run flow for a provider that has no endpoint yet (custom, or any
// user-added provider): seed its dedicated file from the current connection, then
// confirm via the same provider form used everywhere else. Esc leaves the seeded
// file behind for external editing — never a dead end. Which providers exist is
// the plugins' business; this flow only reacts to the state the domain reported.
// Returns false when the user cancelled.
bool SlashDispatcher::seed_new_provider(const std::string& a) {
    // The provider exists -- select() failed only because it has no endpoint --
    // so its definition carries the wire protocol, which the edit form does not
    // edit and a save must not drop.
    agent::Provider existing;
    if (auto p = tui_.providers_->find(a))
        existing = *p;
    agent::Config seed = tui_.cfg_;
    seed.flavor = existing.flavor;
    agent::seed_provider(a, seed);
    agent::Config prov_cfg = seed;
    prov_cfg.provider_name = a;
    if (!edit_provider_form(prov_cfg, "Configure provider " + a)) {
        refresh_provider_feed();
        tui_.append_line(P_STATUS, "provider file created at " + agent::global_config_dir() +
                                       "/providers/" + a +
                                       ".conf \u2014 edit it or re-run /set provider " + a);
        return false;
    }
    tui_.providers_->save(agent::provider_from_edit(existing, prov_cfg, false));
    // Newly configured: its catalogue has never been fetched, and without it
    // the provider contributes no rows to /set model.
    tui_.refresh_provider_catalogs_async();
    refresh_model_list();
    return true;
}

// Key-requiring provider with no key configured: prompt for the key inline (same
// edit form as /settings) so switching never strands the user on a provider that
// cannot authenticate. Esc cancels the switch.
bool SlashDispatcher::prompt_provider_key(const std::string& a, const agent::Provider& p) {
    agent::Config prov_cfg;
    prov_cfg.provider_name = a;
    prov_cfg.api_base = p.api_base;
    prov_cfg.api_key = tui_.cfg_.api_key; // keep an existing global key
    prov_cfg.model = p.default_model;
    if (tui_.cfg_.provider_name == a) {
        prov_cfg.api_base = tui_.cfg_.api_base;
        prov_cfg.api_key = tui_.cfg_.api_key;
        prov_cfg.model = tui_.cfg_.model;
    }
    if (!edit_provider_form(prov_cfg, "Configure: " + a))
        return false;
    tui_.providers_->save(agent::provider_from_edit(p, prov_cfg, p.builtin));
    return true;
}

// Bring `a` to a usable selection: seed a new provider when it is unknown, and
// prompt for a key when the provider needs one. False when the user backed out
// or the selection is unusable.
bool SlashDispatcher::ensure_provider_ready(const std::string& a, agent::ProviderSelection& sel) {
    if (!sel.ok() && sel.error.find("no endpoint") == std::string::npos) {
        tui_.append_line(P_STATUS, "error: " + sel.error);
        return false;
    }
    if (!sel.ok()) {
        if (!seed_new_provider(a))
            return false;
        sel = tui_.providers_->select(a);
    }
    if (!sel.ok()) {
        tui_.append_line(P_STATUS, "error: " + sel.error);
        return false;
    }
    if (sel.provider.requires_key && sel.provider.api_key.empty() && !sel.warning.empty()) {
        if (!prompt_provider_key(a, sel.provider))
            return false;
        sel = tui_.providers_->select(a);
        if (!sel.ok()) {
            tui_.append_line(P_STATUS, "error: " + sel.error);
            return false;
        }
    }
    return true;
}

void SlashDispatcher::cmd_provider(const std::string& a) {
    if (busy_reject("provider"))
        return;
    if (a.empty()) {
        tui_.append_line(P_STATUS, "current provider: " + tui_.cfg_.provider_name + " (" +
                                       tui_.cfg_.api_base + ")");
        return;
    }
    auto sel = tui_.providers_->select(a);
    if (!ensure_provider_ready(a, sel))
        return;

    agent::apply_selection(tui_.cfg_, sel);
    if (!sel.warning.empty())
        tui_.append_line(P_STATUS, "warning: " + sel.warning);
    land_provider_switch();
    tui_.append_line(P_STATUS, "provider switched to " + a + " (model: " + tui_.cfg_.model + ")");
}

// Land a switch that apply_selection() has resolved into cfg_: the window agent
// adopts the connection, the wallet follows the active provider, and the feeds
// are rebuilt against the new endpoint. Shared by /set provider and by /set
// model picking a row from another provider.
void SlashDispatcher::land_provider_switch() {
    // The wallet follows the active provider, so a switch invalidates it.
    tui_.plugin_runtime_.request_wallet_refresh();
    if (tui_.win().agent)
        tui_.win().agent->set_provider(tui_.cfg_, tui_.cfg_.model);
    tui_.cfg_.save_global(agent::global_config_path());
    refresh_provider_feed();
    // The new endpoint has no catalogue yet, and /set model lists the union, so
    // fetch it (and any other configured provider that is cold) and re-merge the
    // feed when they land.
    tui_.refresh_provider_catalogs_async();
    refresh_model_list();
}

void SlashDispatcher::cmd_provider_list() {
    tui_.show_provider_list();
}

bool SlashDispatcher::adopt_provider(const std::string& id) {
    auto sel = tui_.providers_->select(id);
    if (!sel.ok()) {
        tui_.append_line(P_STATUS, "provider " + id + ": " + sel.error);
        return false;
    }
    agent::apply_selection(tui_.cfg_, sel);
    if (!sel.warning.empty())
        tui_.append_line(P_STATUS, "warning: " + sel.warning);
    land_provider_switch();
    return true;
}

// --- plugin runtime surface (/get plugin, /set plugin) --------------------
// One line per plugin, fixed column order (id, tier, state, contributions) so
// the output is both readable and stable enough to assert on.

namespace {

std::string plugin_state_word(bool enabled) {
    return enabled ? "on" : "off";
}

// Capability-kind names come from the core (plugin_console), so this command
// and the registry console cannot describe the same plugin differently.

} // namespace

void SlashDispatcher::cmd_runtime_plugin_list() {
    // One formatter for the command and the registry console: what the user
    // reads in the scrollback and what the panel shows cannot drift.
    for (const auto& line : agent::plugin_console_lines(tui_.plugin_runtime_))
        tui_.append_line(P_STATUS, line);
}

void SlashDispatcher::cmd_runtime_plugin_get(const std::string& id) {
    if (id.empty() || id == "list") {
        cmd_runtime_plugin_list();
        return;
    }
    tui_.show_plugin(id);
}

// The wallet: one readout for every provider, showing the ACTIVE provider's
// account state. A display preference, so it is a single on/off rather than a
// per-provider setting.
//
// The bar carries one number because width is the constraint; this carries the
// whole picture - the plan, each metered window and any prepaid balance. That
// division is why one mechanism needs one command pair instead of two.
namespace {

// Why a wallet cannot be shown, or "" when it is usable.
std::string wallet_unavailable_reason(const agent::WalletView& wallet) {
    if (!wallet.supported)
        return wallet.holder + " declares no wallet";
    if (wallet.failed)
        return wallet.holder + ": unavailable — check the key";
    if (!wallet.ready)
        return wallet.holder + ": not fetched yet";
    return "";
}

// One usage window row: label, percent used, remaining/entitlement, reset.
std::string wallet_window_row(const agent::WalletWindow& w, const std::string& unit) {
    std::string row = "  " + w.label;
    if (w.percent_used >= 0)
        row += "  " + std::to_string(static_cast<int>(w.percent_used)) + "% used";
    if (w.remaining >= 0 && w.entitlement >= 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  %.2f/%.2f %s", w.remaining, w.entitlement, unit.c_str());
        row += buf;
    }
    if (!w.resets_at.empty())
        row += "  resets " + w.resets_at;
    return row;
}

} // namespace

void SlashDispatcher::cmd_get_wallet() {
    const auto wallet = tui_.plugin_runtime_.wallet();
    std::string line = std::string("provider wallet: ") + (wallet.enabled ? "on" : "off");
    if (const std::string why = wallet_unavailable_reason(wallet); !why.empty()) {
        tui_.append_line(P_STATUS, line + "  (" + why + ")");
        return;
    }

    const agent::WalletSnapshot& snapshot = wallet.snapshot;
    if (!snapshot.plan.empty())
        line += "  [" + snapshot.plan + "]";
    tui_.append_line(P_STATUS, line + "  " + wallet.holder);

    const std::string unit = snapshot.currency.empty() ? snapshot.unit : snapshot.currency;
    for (const auto& w : snapshot.windows)
        tui_.append_line(P_STATUS, wallet_window_row(w, unit));
    if (snapshot.credits_balance) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  balance: %.2f %s", *snapshot.credits_balance,
                      unit.c_str());
        tui_.append_line(P_STATUS, buf);
    }
}

void SlashDispatcher::cmd_set_wallet(const std::string& val) {
    bool enabled;
    if (val == "on")
        enabled = true;
    else if (val == "off")
        enabled = false;
    else if (val == "toggle")
        enabled = !tui_.cfg_.wallet_enabled;
    else {
        tui_.append_line(P_STATUS, "usage: /set provider wallet on|off|toggle (got: " + val + ")");
        return;
    }
    tui_.cfg_.wallet_enabled = enabled;
    tui_.cfg_.save_global(agent::global_config_path());
    // Turning it on should show a number, not "not fetched yet".
    if (enabled)
        tui_.plugin_runtime_.request_wallet_refresh();
    tui_.append_line(P_STATUS, std::string("provider wallet ") + (enabled ? "on" : "off"));
}

void SlashDispatcher::show_plugin(const std::string& id) {
    if (!tui_.plugin_runtime_.has(id)) {
        tui_.append_line(P_STATUS, "unknown plugin: " + id);
        return;
    }
    const auto status = tui_.plugin_runtime_.status(id);
    tui_.append_line(P_STATUS, "plugin " + status.id + ": " + plugin_state_word(status.enabled) +
                                   " (" + status.category + ", " + status.tier + " v" +
                                   status.version + ")");
    if (!status.description.empty())
        tui_.append_line(P_STATUS, "  " + status.description);
    if (status.contributions.empty()) {
        tui_.append_line(P_STATUS, "  contributes nothing");
    }
    for (const auto& item : status.contributions) {
        tui_.append_line(P_STATUS, "  " + std::string(agent::capability_kind_name(item.kind)) +
                                       ": " + item.name);
    }
    // External plugins also carry a manifest: who wrote them, where they are
    // installed, and what went wrong when they did not load.
    const agent::PluginInfo* info = tui_.plugins_.find(id);
    if (!info)
        return;
    tui_.append_line(P_STATUS, "  " + info->manifest.name + " (" + info->manifest.author + ")");
    if (!info->manifest.url.empty())
        tui_.append_line(P_STATUS, "  url: " + info->manifest.url);
    if (!info->manifest.license.empty())
        tui_.append_line(P_STATUS, "  license: " + info->manifest.license);
    tui_.append_line(P_STATUS, "  dir: " + info->dir);
    tui_.append_line(P_STATUS, "  tools: " + std::to_string(info->manifest.tools.size()));
    if (!info->error.empty())
        tui_.append_line(P_STATUS, "  error: " + info->error);
}

namespace {

// " (removed 9 tool, 1 segment)" — what a disable actually took away, grouped
// by capability kind so the count is legible at a glance.
std::string removed_summary(const std::vector<agent::ExtensionItem>& items) {
    if (items.empty())
        return " (contributed nothing)";
    std::vector<std::pair<std::string, int>> counts;
    for (const auto& item : items) {
        const std::string kind = agent::capability_kind_name(item.kind);
        auto it = std::find_if(counts.begin(), counts.end(),
                               [&kind](const auto& c) { return c.first == kind; });
        if (it == counts.end())
            counts.emplace_back(kind, 1);
        else
            ++it->second;
    }
    std::string out = " (removed ";
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (i)
            out += ", ";
        out += std::to_string(counts[i].second) + " " + counts[i].first;
    }
    return out + ")";
}

} // namespace

void SlashDispatcher::set_plugin(const std::string& id, bool on) {
    if (!tui_.plugin_runtime_.has(id)) {
        tui_.append_line(P_STATUS, "unknown plugin: " + id);
        return;
    }
    const auto before = tui_.plugin_runtime_.status(id);
    if (before.enabled == on) {
        tui_.append_line(P_STATUS, "plugin " + id + (on ? " is already on" : " is already off"));
        return;
    }
    if (!tui_.plugin_runtime_.set_state(id, on)) {
        tui_.append_line(P_STATUS,
                         "plugin " + id + (on ? ": could not enable" : ": could not disable"));
        return;
    }
    // Say what a disable took away: losing the core tool set leaves an agent
    // that can read skills but cannot touch the filesystem, and that is worth
    // seeing at the moment it happens rather than when the next turn stalls.
    // (The registry is never empty: the agent always registers the skill tools,
    // so there is no "no tools at all" state to warn about.)
    tui_.append_line(P_STATUS, "plugin " + id + (on ? " on" : " off") +
                                   (on ? "" : removed_summary(before.contributions)));
    // A plugin may contribute a slash-command namespace: rebuild the tree so
    // its commands appear or disappear immediately, with no restart.
    refresh_completions();
    // What that change means for the set as a whole: a capability the harness
    // assumes is now unperformable, or two tools competing for one job. The
    // line above says what left; this says what it costs.
    report_toolset_audit();
}

void SlashDispatcher::report_toolset_audit() {
    for (const auto& finding : tui_.plugin_runtime_.audit()) {
        tui_.append_line(P_STATUS,
                         "warning (" + agent::to_string(finding.kind) + "): " + finding.message);
    }
}

void SlashDispatcher::cmd_provider_delete(const std::string& name) {
    if (name.empty()) {
        tui_.append_line(P_STATUS, "usage: /provider delete <name>");
        return;
    }
    tui_.providers_->remove(name);
    refresh_provider_feed();
    tui_.append_line(P_STATUS, "deleted provider: " + name);
}

void SlashDispatcher::cmd_provider_test(const std::string& name) {
    if (name.empty()) {
        tui_.append_line(P_STATUS, "usage: /provider test <name>");
        return;
    }
    auto p = tui_.providers_->find(name);
    if (!p) {
        tui_.append_line(P_STATUS, name + ": unknown provider");
        return;
    }
    if (p->api_base.empty()) {
        tui_.append_line(P_STATUS, name + ": FAILED (no endpoint configured)");
        return;
    }
    tui_.append_line(P_STATUS, "testing " + name + "...");
    agent::Config pc;
    pc.api_base = p->api_base;
    pc.api_key = p->api_key;
    pc.flavor = p->flavor;
    agent::model_catalog_refresh_async(
        pc, tui_.ui_poster(), [this, name, pc](const agent::CatalogFetchReport& r) {
            const bool ok = r.fetched && !agent::list_model_info_cached(pc).empty();
            tui_.append_line(P_STATUS,
                             name + ": " + (ok ? "OK" : "FAILED (" + r.failure_reason() + ")"));
        });
}

void SlashDispatcher::cmd_session_load(const std::string& id) {
    if (id.empty()) {
        tui_.session_controller_->session_browser();
        return;
    }
    agent::Session sess;
    tui_.session_controller_->store().load(id, sess);
    if (sess.id.empty())
        tui_.append_line(P_STATUS, "session not found: " + id);
    else
        tui_.load_session(id);
}

void SlashDispatcher::cmd_session_delete(const std::string& id) {
    if (id.empty()) {
        tui_.append_line(P_STATUS, "usage: /session remove <id>");
        return;
    }
    tui_.session_controller_->store().remove(id);
    tui_.append_line(P_STATUS, "deleted session: " + id);
}

void SlashDispatcher::cmd_session_rename(const std::string& rest) {
    auto trim = [](const std::string& s) {
        size_t b = s.find_first_not_of(" \t");
        size_t e = s.find_last_not_of(" \t");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    size_t sp = rest.find(' ');
    std::string id = trim(sp == std::string::npos ? rest : rest.substr(0, sp));
    std::string title = sp == std::string::npos ? "" : trim(rest.substr(sp + 1));
    if (id.empty() || title.empty()) {
        tui_.append_line(P_STATUS, "usage: /session rename <id> <title>");
        return;
    }
    agent::Session sess;
    if (!tui_.session_controller_->store().load(id, sess)) {
        tui_.append_line(P_STATUS, "session not found: " + id);
        return;
    }
    sess.title = title;
    if (!tui_.session_controller_->store().save(sess))
        tui_.append_line(P_STATUS, "failed to rename session: " + id);
    else
        tui_.append_line(P_STATUS, "renamed session " + id + " to \"" + title + "\"");
}

void SlashDispatcher::cmd_files_ls(const std::string& rest) {
    namespace fs = std::filesystem;
    std::string path, err;
    if (!tui::confine_path(rest.empty() ? "." : rest, path, err)) {
        tui_.append_line(P_STATUS, "path denied: " + err);
        return;
    }
    fs::path p(path);
    if (!fs::exists(p)) {
        tui_.append_line(P_STATUS, "not found: " + path);
        return;
    }
    if (fs::is_directory(p)) {
        std::string out;
        for (const auto& e : fs::directory_iterator(p))
            out += e.path().filename().string() + "  ";
        if (out.empty())
            out = "(empty)";
        tui_.append_line(P_ASSISTANT, out);
    } else {
        tui_.append_line(P_ASSISTANT, p.filename().string());
    }
}

void SlashDispatcher::cmd_files_tree(const std::string& rest) {
    namespace fs = std::filesystem;
    std::string path, err;
    if (!tui::confine_path(rest.empty() ? "." : rest, path, err)) {
        tui_.append_line(P_STATUS, "path denied: " + err);
        return;
    }
    fs::path p(path);
    std::string out;
    if (fs::exists(p) && fs::is_directory(p)) {
        std::function<void(const fs::path&, int)> walk;
        walk = [&](const fs::path& dir, int depth) {
            if (depth > 3)
                return;
            for (const auto& e : fs::directory_iterator(dir)) {
                for (int i = 0; i < depth; ++i)
                    out += "  ";
                out += e.path().filename().string() + "\n";
                if (fs::is_directory(e))
                    walk(e.path(), depth + 1);
            }
        };
        walk(p, 0);
    }
    if (out.empty())
        out = "(empty)";
    tui_.append_line(P_ASSISTANT, out);
}

void SlashDispatcher::cmd_files_open(const std::string& rest) {
    namespace fs = std::filesystem;
    std::string path, err;
    if (!tui::confine_path(rest.empty() ? "." : rest, path, err)) {
        tui_.append_line(P_STATUS, "path denied: " + err);
        return;
    }
    fs::path p(path);
    if (!fs::exists(p) || fs::is_directory(p)) {
        tui_.append_line(P_STATUS, "not a file: " + path);
        return;
    }
    std::ifstream f(p);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.size() > 4096)
        content.resize(4096);
    tui_.append_line(P_ASSISTANT, p.filename().string() + ":\n" + content);
}

void SlashDispatcher::cmd_files_find(const std::string& rest) {
    namespace fs = std::filesystem;
    std::string path, err;
    if (!tui::confine_path(rest.empty() ? "." : rest, path, err)) {
        tui_.append_line(P_STATUS, "path denied: " + err);
        return;
    }
    fs::path p(path);
    if (!fs::exists(p) || !fs::is_directory(p)) {
        tui_.append_line(P_STATUS, "not a directory: " + path);
        return;
    }
    for (const auto& e : fs::directory_iterator(p))
        tui_.append_line(P_ASSISTANT, e.path().filename().string());
}

void SlashDispatcher::cmd_system_exec(const std::string& rest) {
    if (rest.empty()) {
        tui_.append_line(P_STATUS, "usage: /system exec <command>");
        return;
    }
    // Route through JobService so the command is visible in /jobs, killable,
    // timeout-bounded, and output-capped — no untracked raw popen. The UI thread
    // starts it and returns; the output lands when the job finishes.
    tui_.run_command_async("exec", rest);
}

void SlashDispatcher::cmd_system_delete(const std::string& rest) {
    if (rest.empty()) {
        tui_.append_line(P_STATUS, "usage: /system delete <path>");
        return;
    }
    std::string resolved, err;
    if (!agent::Workspace::confine(rest, resolved, err)) {
        tui_.append_line(P_STATUS, "delete denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(resolved, ec);
    tui_.append_line(P_STATUS, ec ? ("delete failed: " + ec.message()) : ("deleted: " + rest));
}

void SlashDispatcher::cmd_system_rmdir(const std::string& rest) {
    if (rest.empty()) {
        tui_.append_line(P_STATUS, "usage: /system rmdir <path>");
        return;
    }
    std::string resolved, err;
    if (!agent::Workspace::confine(rest, resolved, err)) {
        tui_.append_line(P_STATUS, "rmdir denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::remove(resolved, ec);
    tui_.append_line(P_STATUS, ec ? ("rmdir failed: " + ec.message()) : ("removed dir: " + rest));
}

void SlashDispatcher::cmd_system_mkdir(const std::string& rest) {
    if (rest.empty()) {
        tui_.append_line(P_STATUS, "usage: /system mkdir <path>");
        return;
    }
    std::string resolved, err;
    if (!agent::Workspace::confine(rest, resolved, err)) {
        tui_.append_line(P_STATUS, "mkdir denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(resolved, ec);
    tui_.append_line(P_STATUS, ec ? ("mkdir failed: " + ec.message()) : ("created: " + rest));
}

void SlashDispatcher::cmd_system_mv(const std::string& rest) {
    if (rest.empty()) {
        tui_.append_line(P_STATUS, "usage: /system mv <src> <dst>");
        return;
    }
    size_t sp = rest.find(' ');
    if (sp == std::string::npos) {
        tui_.append_line(P_STATUS, "usage: /system mv <src> <dst>");
        return;
    }
    std::string src, dst, err;
    if (!agent::Workspace::confine(rest.substr(0, sp), src, err) ||
        !agent::Workspace::confine(rest.substr(sp + 1), dst, err)) {
        tui_.append_line(P_STATUS, "mv denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::rename(src, dst, ec);
    tui_.append_line(P_STATUS, ec ? ("mv failed: " + ec.message()) : "moved");
}

void SlashDispatcher::cmd_system_cp(const std::string& rest) {
    if (rest.empty()) {
        tui_.append_line(P_STATUS, "usage: /system cp <src> <dst>");
        return;
    }
    size_t sp = rest.find(' ');
    if (sp == std::string::npos) {
        tui_.append_line(P_STATUS, "usage: /system cp <src> <dst>");
        return;
    }
    std::string src, dst, err;
    if (!agent::Workspace::confine(rest.substr(0, sp), src, err) ||
        !agent::Workspace::confine(rest.substr(sp + 1), dst, err)) {
        tui_.append_line(P_STATUS, "cp denied: " + err);
        return;
    }
    std::error_code ec;
    std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive, ec);
    tui_.append_line(P_STATUS, ec ? ("cp failed: " + ec.message()) : "copied");
}

void SlashDispatcher::cmd_system_info(const std::string& rest) {
    if (rest.empty()) {
        tui_.append_line(P_STATUS, "usage: /system info <path>");
        return;
    }
    std::string resolved, err;
    if (!agent::Workspace::confine(rest, resolved, err)) {
        tui_.append_line(P_STATUS, "info denied: " + err);
        return;
    }
    std::error_code ec;
    auto st = std::filesystem::status(resolved, ec);
    if (ec) {
        tui_.append_line(P_STATUS, "info failed: " + ec.message());
        return;
    }
    tui_.append_line(P_STATUS, rest + ": " + (std::filesystem::is_directory(st) ? "dir" : "file"));
}

void SlashDispatcher::cmd_system_ps() {
    tui_.run_command_async("ps", "ps -eo pid,comm,args --no-headers | head -30");
}

void SlashDispatcher::cmd_system_kill(const std::string& rest) {
    if (rest.empty()) {
        tui_.append_line(P_STATUS, "usage: /system kill <pid>");
        return;
    }
    auto pid = static_cast<pid_t>(std::atoi(rest.c_str()));
    if (pid <= 0) {
        tui_.append_line(P_STATUS, "invalid pid: " + rest);
        return;
    }
    tui_.append_line(P_STATUS, kill(pid, SIGKILL) == 0 ? ("killed " + rest) : "kill failed");
}

void SlashDispatcher::cmd_system_df() {
    tui_.run_command_async("df", "df -h | head -20");
}

void SlashDispatcher::cmd_system_uptime() {
    tui_.run_command_async("uptime", "uptime");
}

void SlashDispatcher::cmd_system_uname() {
    tui_.run_command_async("uname", "uname -a");
}

namespace {

struct WorkItem {
    std::string kind;
    std::string id;
    std::string label;
    std::string project;
    std::size_t window_index = 0;
};

std::vector<WorkItem> collect_work_items(tui::WindowManager& windows, agent::JobService& jobs,
                                         agent::SubAgentExecutor& subagents,
                                         agent::PluginRuntime& runtime) {
    std::vector<WorkItem> items;
    std::map<std::string, std::string> owner_project;
    const auto& all = windows.all();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const tui::Window& w = *all[i];
        if (w.kind == tui::Window::Kind::Resource || w.welcome_art)
            continue;
        const std::string project =
            w.project_root.empty() ? agent::Workspace::project_root() : w.project_root;
        owner_project[w.owner_id] = project;
        items.push_back({"session", std::to_string(w.id), w.title, project, i});
    }
    const auto project_for = [&](const std::string& owner) {
        const auto it = owner_project.find(owner);
        return it == owner_project.end() ? agent::Workspace::process_project_root() : it->second;
    };
    for (const auto& job : jobs.list())
        items.push_back({"job", job.id, job.label, project_for(job.owner_id), 0});
    for (const auto& task : subagents.tasks())
        items.push_back({"agent", task.id, task.prompt_head, project_for(task.owner_id), 0});
    if (auto* watch = dynamic_cast<agent::plugins::WatchPlugin*>(runtime.find("watch")))
        for (const auto& record : watch->list_records())
            items.push_back({"watch", record.id, record.label, project_for(record.owner_id), 0});
    return items;
}

std::string work_label(const WorkItem& item) {
    const std::string project = item.project.empty() ? "" : "  [" + item.project + "]";
    if (item.kind == "session")
        return "session  " + item.label + project;
    if (item.kind == "job")
        return "job      " + item.id + "  " + item.label + project;
    if (item.kind == "agent")
        return "agent    " + item.id + "  " + item.label + project;
    return "watch    " + item.id + "  " + item.label + project;
}

} // namespace

namespace {

void print_command_list(const std::vector<palette::Command>& commands,
                        const std::function<void(const std::string&)>& out) {
    size_t width = 0;
    for (const auto& c : commands)
        width = std::max(width, palette::usage(c).size());
    for (const auto& c : commands) {
        std::string usage_line = palette::usage(c);
        usage_line.append(width - usage_line.size() + 2, ' ');
        std::string line = "  " + usage_line + c.help;
        if (!c.short_name.empty())
            line += "  (/" + c.short_name + ")";
        out(line);
    }
    std::string shortcuts;
    for (const auto& c : commands) {
        if (c.short_name.empty())
            continue;
        if (!shortcuts.empty())
            shortcuts += "  ";
        shortcuts += "/" + c.short_name + "=" + c.name;
    }
    if (!shortcuts.empty())
        out("Shortcuts: " + shortcuts);
    out("Glued paths: /wc = /window close, /sl = /session list");
}

} // namespace

namespace {

bool work_in_section(const WorkItem& item, int section) {
    if (section == 0)
        return true;
    if (section == 1)
        return item.kind == "session";
    if (section == 2)
        return item.kind == "job";
    if (section == 3)
        return item.kind == "agent";
    return item.kind == "watch";
}

std::vector<WorkItem> visible_work(const std::vector<WorkItem>& items, int section,
                                   const std::string& filter) {
    std::vector<WorkItem> out;
    for (const auto& item : items) {
        if (!work_in_section(item, section))
            continue;
        if (!filter.empty() && !tui::drawer_row_matches(work_label(item), filter))
            continue;
        out.push_back(item);
    }
    return out;
}

std::string work_key(const WorkItem& item) {
    return item.kind + ":" + item.id;
}

struct WorkBoardState {
    std::vector<WorkItem> all;
    std::vector<WorkItem> items;
    int section = 0;
    int sel = 0;
    std::string filter;
    std::string selected_key;
    bool filtering = false;

    void sync(const std::vector<WorkItem>& fresh) {
        all = fresh;
        items = visible_work(all, section, filter);
        if (!selected_key.empty()) {
            sel = 0;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (work_key(items[i]) == selected_key) {
                    sel = static_cast<int>(i);
                    break;
                }
            }
        }
        if (items.empty())
            sel = 0;
        else
            sel = std::min(std::max(0, sel), static_cast<int>(items.size()) - 1);
    }

    const WorkItem* current() const {
        return items.empty() ? nullptr : &items[static_cast<std::size_t>(sel)];
    }

    void cycle(int delta) {
        section = (section + delta + 5) % 5;
        sel = 0;
        selected_key.clear();
    }

    void move(int delta) { sel = std::max(0, sel + delta); }
};

void draw_work_board(WINDOW* w, int height, int width, const WorkBoardState& state) {
    werase(w);
    const std::string tabs = std::string(state.section == 0 ? "[" : " ") + "All" +
                             (state.section == 0 ? "]" : " ") + "  " +
                             (state.section == 1 ? "[Sessions]" : " Sessions ") + "  " +
                             (state.section == 2 ? "[Jobs]" : " Jobs ") + "  " +
                             (state.section == 3 ? "[Agents]" : " Agents ") + "  " +
                             (state.section == 4 ? "[Watches]" : " Watches ");
    wattron(w, COLOR_PAIR(P_BANNER) | A_BOLD);
    mvwaddnstr(w, 0, 0, tabs.c_str(), width);
    wattroff(w, COLOR_PAIR(P_BANNER) | A_BOLD);
    int row = 1;
    const int list_h = height - 2;
    const int first = std::max(0, state.sel - list_h + 1);
    for (int i = first; i < static_cast<int>(state.items.size()) && row < list_h; ++i, ++row) {
        const bool current = i == state.sel;
        if (current)
            wattron(w, A_REVERSE);
        mvwaddnstr(w, row, 0, work_label(state.items[static_cast<std::size_t>(i)]).c_str(), width);
        if (current)
            wattroff(w, A_REVERSE);
    }
    if (state.items.empty())
        mvwaddnstr(w, 1, 0, "(no matching work)", width);
    wattron(w, COLOR_PAIR(P_STATUS));
    const std::string line = state.filtering ? "/" + state.filter : "Press / to filter";
    mvwaddnstr(w, height - 1, 0, line.c_str(), width);
    wattroff(w, COLOR_PAIR(P_STATUS));
    wnoutrefresh(w);
}

bool work_board_filter_key(int ch, WorkBoardState& state) {
    if (ch == 27 || ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        state.filtering = false;
    } else if (ch == 127 || ch == KEY_BACKSPACE) {
        if (!state.filter.empty())
            state.filter.pop_back();
    } else if (ch >= 32 && ch <= 126) {
        state.filter.push_back(static_cast<char>(ch));
    }
    return true;
}

bool work_board_nav_key(int ch, WorkBoardState& state) {
    switch (ch) {
    case '\t':
        state.cycle(1);
        return true;
    case KEY_BTAB:
        state.cycle(-1);
        return true;
    case KEY_UP:
        state.move(-1);
        return true;
    case KEY_DOWN:
        state.move(1);
        return true;
    case KEY_PPAGE:
        state.move(-10);
        return true;
    case KEY_NPAGE:
        state.move(10);
        return true;
    case '/':
        state.filtering = true;
        state.filter.clear();
        return true;
    default:
        return false;
    }
}

enum class WorkBoardAction { None, Close, Open, Stop, NewSession };

WorkBoardAction work_board_command_key(int ch, const WorkBoardState& state) {
    switch (ch) {
    case 27:
        return WorkBoardAction::Close;
    case '\n':
    case '\r':
    case KEY_ENTER:
    case 'a':
    case 'r':
        return state.current() ? WorkBoardAction::Open : WorkBoardAction::None;
    case 'x': {
        const WorkItem* item = state.current();
        return item && (item->kind == "job" || item->kind == "watch") ? WorkBoardAction::Stop
                                                                      : WorkBoardAction::None;
    }
    case 'n':
        return WorkBoardAction::NewSession;
    default:
        return WorkBoardAction::None;
    }
}

WorkBoardAction work_board_key(int ch, WorkBoardState& state) {
    if (state.filtering) {
        work_board_filter_key(ch, state);
        return WorkBoardAction::None;
    }
    if (work_board_nav_key(ch, state))
        return WorkBoardAction::None;
    return work_board_command_key(ch, state);
}

} // namespace

void SlashDispatcher::cmd_work_board() {
    Dialog dlg(std::min(26, std::max(8, LINES - 2)), std::min(120, std::max(40, COLS - 4)), "Work");
    dlg.set_footer({{"Enter", "open"},
                    {"a", "attach"},
                    {"x", "stop"},
                    {"Tab", "section"},
                    {"/", "filter"},
                    {"Esc", "close"}});
    WINDOW* w = dlg.content();
    const int height = dlg.rows() - 2;
    const int width = dlg.cols() - 2;
    wtimeout(w, 250);
    WorkBoardState state;

    bool done = false;
    while (!done) {
        state.sync(collect_work_items(*tui_.window_manager_, tui_.jobs_, tui_.subagents_,
                                      tui_.plugin_runtime_));
        draw_work_board(w, height, width, state);
        update_panels();
        doupdate();

        const int ch = wgetch(w);
        if (ch == ERR || ch == KEY_RESIZE)
            continue;
        const WorkBoardAction action = work_board_key(ch, state);
        if (action == WorkBoardAction::Close) {
            done = true;
        } else if (action == WorkBoardAction::NewSession) {
            tui_.new_window("chat");
            done = true;
        } else if (action == WorkBoardAction::Open || action == WorkBoardAction::Stop) {
            if (const WorkItem* item = state.current()) {
                apply_work_item(item->kind, item->id, item->window_index);
                done = true;
            }
        }
    }
    dlg.hide();
    tui_.draw();
}

void SlashDispatcher::cmd_usage_show() {
    std::vector<std::pair<std::string, agent::UsageTotals>> windows;
    for (const auto& window : tui_.window_manager_->all())
        if (window->kind == tui::Window::Kind::Chat)
            windows.emplace_back(window->title, window->usage);
    tui_.append_line(P_STATUS, "usage - session tree");
    for (const auto& line :
         tui::usage_report_lines(tui_.win().ledger, tui_.subagents_.tasks(), windows))
        tui_.append_line(P_STATUS, "  " + line);
    tui_.draw();
}

void SlashDispatcher::cmd_usage_reset() {
    tui_.win().ledger.reset();
    tui_.win().usage = tui_.win().ledger.tree();
    tui_.append_line(P_STATUS, "usage: session tree reset");
    tui_.draw();
}

void SlashDispatcher::open_resource(const std::string& kind, const std::string& id) {
    if (kind.empty() || id.empty()) {
        tui_.append_line(P_STATUS, "usage: /window attach <job|agent|watch> <id>");
        return;
    }
    if (!tui_.window_manager_->focus_resource(kind, id))
        tui_.window_manager_->new_resource_window(kind, id, kind + " " + id);
    tui_.refresh_active_resource();
    tui_.draw();
}

void SlashDispatcher::apply_work_item(const std::string& kind, const std::string& id,
                                      std::size_t window_index) {
    if (kind == "session") {
        tui_.switch_to(window_index);
        return;
    }
    std::vector<std::string> actions = {"open viewer"};
    if (kind == "job" || kind == "watch")
        actions.emplace_back("stop");
    const int action = menu_select(kind + " " + id, actions);
    if (action == 0) {
        open_resource(kind, id);
    } else if (action == 1 && kind == "job") {
        tui_.jobs_.stop(id);
        tui_.append_line(P_STATUS, "stopped job " + id);
        tui_.draw();
    } else if (action == 1 && kind == "watch") {
        if (auto* watch =
                dynamic_cast<agent::plugins::WatchPlugin*>(tui_.plugin_runtime_.find("watch"))) {
            watch->stop_watch(id);
            tui_.append_line(P_STATUS, "stopped watch " + id);
        }
        tui_.draw();
    }
}

void SlashDispatcher::cmd_project_list() {
    tui_.banner("Open projects:");
    for (const auto& window : tui_.window_manager_->all()) {
        if (window->kind == Window::Kind::Resource)
            continue;
        const std::string root =
            window->project_root.empty() ? agent::Workspace::project_root() : window->project_root;
        tui_.append_line(P_STATUS, "  " + window->title + "  ->  " + root);
    }
}

void SlashDispatcher::cmd_project_open(const std::string& args) {
    if (args.empty()) {
        tui_.append_line(P_STATUS,
                         "usage: /project open <path> [--worktree <name>] [--no-worktree]");
        return;
    }
    const std::size_t sp = args.find(' ');
    const std::string path = sp == std::string::npos ? args : args.substr(0, sp);
    bool prepare_workspace = tui_.cfg_.worktree_auto;
    std::string worktree_name;
    if (sp != std::string::npos) {
        const std::string flags = args.substr(sp + 1);
        if (flags == "--no-worktree") {
            prepare_workspace = false;
        } else if (flags.rfind("--worktree ", 0) == 0) {
            prepare_workspace = true;
            worktree_name = flags.substr(11);
        } else if (!flags.empty()) {
            tui_.append_line(P_STATUS, "usage: /project open <path> [--worktree <name>] "
                                       "[--no-worktree]");
            return;
        }
    }
    std::error_code ec;
    std::filesystem::path requested(path);
    if (requested.is_relative())
        requested = std::filesystem::absolute(requested, ec);
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(requested, ec);
    const std::string root = (ec ? requested : canonical).generic_string();
    if (!std::filesystem::is_directory(root, ec)) {
        tui_.append_line(P_STATUS, "project: not a directory: " + root);
        return;
    }
    Window& w = tui_.new_window("project", prepare_workspace, worktree_name, root);
    w.title = std::filesystem::path(root).filename().string();
    tui_.append_line(P_STATUS, "project opened: " + root);
    tui_.draw();
}

void SlashDispatcher::cmd_project_show() {
    const std::string root = tui_.win().project_root.empty() ? agent::Workspace::project_root()
                                                             : tui_.win().project_root;
    tui_.append_line(P_STATUS, "project: " + root);
}

void SlashDispatcher::cmd_project_close() {
    if (tui_.win().kind == Window::Kind::Resource) {
        tui_.append_line(P_STATUS, "project close: active window is a resource viewer");
        return;
    }
    tui_.close_window();
}

void SlashDispatcher::cmd_help(const std::string& arg) {
    if (arg.empty()) {
        tui_.banner("Slash commands (type /help <command> for detail):");
        print_command_list(commands(),
                           [this](const std::string& line) { tui_.append_line(P_STATUS, line); });
        tui_.append_line(P_STATUS, "");
        tui_.append_line(P_STATUS,
                         "Keys:  Enter/Ctrl-G send   PgUp/PgDn scroll   Ctrl+P/N history");
        tui_.append_line(P_STATUS, "       Ctrl-N new window   Ctrl-W close   Alt+1..9 switch");
        tui_.append_line(P_STATUS, "       Ctrl-C quit");
        tui_.append_line(P_STATUS, "Type '/' to open the command drawer (filter, Tab, Enter).");
        tui_.draw();
        return;
    }
    std::string name = arg;
    if (!name.empty() && name[0] == '/')
        name = name.substr(1);
    const Command* c = find_command(name);
    if (!c) {
        tui_.append_line(P_STATUS, "no such command: /" + name);
        return;
    }
    tui_.banner(usage(*c));
    tui_.append_line(P_STATUS, "  " + c->help);
    if (!c->aliases.empty()) {
        std::string al = "  aliases:";
        for (const auto& a : c->aliases)
            al += " /" + a;
        tui_.append_line(P_STATUS, al);
    }
    tui_.draw();
}

// /set worktree: whether a new window prepares its own git worktree. Kept as a
// tree node rather than a hardcoded command check, like every other setting.
void SlashDispatcher::cmd_set_worktree(const std::string& val) {
    if (!valid_toggle(val)) {
        tui_.append_line(P_STATUS, "usage: /set worktree on|off|toggle (got: " + val + ")");
        return;
    }
    const bool on = parse_toggle(val, tui_.cfg_.worktree_auto);
    tui_.cfg_.worktree_auto = on;
    tui_.append_line(P_STATUS, std::string("worktree: ") + (on ? "on" : "off") +
                                   (on ? " \u2014 a new window prepares its own working copy"
                                       : " \u2014 new windows work in the project root"));
    if (!tui_.cfg_.save_settings(tui_.session_controller_->settings_path()))
        tui_.append_line(P_STATUS,
                         "warning: could not save to " + tui_.session_controller_->settings_path());
    // The setting is the default for NEW windows, but turning it off is also
    // the user's request for THIS one to stop sitting in a worktree. Ask, with
    // the full leave flow (cancel, stash, branch) rather than yanking the
    // directory out from under uncommitted work.
    if (!on && !tui_.win().worktree.empty())
        tui_.leave_window_workspace(tui_.win());
    tui_.draw();
}

void SlashDispatcher::cmd_get_worktree() {
    tui_.append_line(P_STATUS,
                     std::string("worktree: ") + (tui_.cfg_.worktree_auto ? "on" : "off"));
}

void SlashDispatcher::refresh_worktree_feed() {
    tui_.refresh_worktree_feed();
}

// Parse "new [name] [at <ref>]". The optional name and the optional ref may
// appear in either order, but only one of each; anything else is a usage error.
bool SlashDispatcher::parse_worktree_new_arg(const std::string& arg, std::string& name,
                                             std::string& ref) {
    ref = "HEAD";
    for (const auto& word : split_words(arg)) {
        if (word == "at") {
            ref.clear(); // the next word is the ref; a missing one is caught below
        } else if (ref.empty()) {
            ref = word;
        } else if (name.empty()) {
            name = word;
        } else {
            tui_.append_line(P_STATUS, "usage: /worktree new [name] [at <ref>]");
            return false;
        }
    }
    if (ref.empty()) {
        tui_.append_line(P_STATUS, "usage: /worktree new [name] [at <ref>]");
        return false;
    }
    return true;
}

// A name is the directory under the shared worktree base; validate it and
// refuse a registry collision. Omitting the name derives one from the window.
bool SlashDispatcher::resolve_worktree_name(const Window& w, const std::string& requested,
                                            std::string& name) {
    name = requested.empty() ? "w" + std::to_string(w.id) : requested;
    if (!agent::valid_worktree_name(name)) {
        tui_.append_line(P_STATUS, "worktree: invalid name: " + name);
        return false;
    }
    const auto entries = agent::load_worktrees(agent::Workspace::state_dir());
    const bool taken = std::any_of(entries.begin(), entries.end(),
                                   [&](const auto& entry) { return entry.name == name; });
    if (taken) {
        std::string message = "worktree: ";
        message += name;
        message += " already exists - attach it with /worktree attach ";
        message += name;
        tui_.append_line(P_STATUS, message);
        return false;
    }
    return true;
}

// Create the directory and its registry row, but leave the active window
// alone. Attaching is a separate step, so a failed create cannot park the
// workspace the window is already using.
bool SlashDispatcher::create_worktree_record(const std::string& name, const std::string& path,
                                             const std::string& base) {
    const std::string project = agent::Workspace::project_root();
    std::string err;
    if (!agent::create_worktree(project, path, base, err)) {
        tui_.append_line(P_STATUS, "worktree: could not create " + path + " - " + err);
        return false;
    }
    if (!agent::registry_add(agent::Workspace::state_dir(),
                             agent::make_worktree_entry(name, path, base, "")))
        tui_.append_line(P_STATUS, "warning: the workspace registry could not be written");
    return true;
}

// /worktree new [name] [at <ref>]: create a detached worktree and attach it.
void SlashDispatcher::cmd_worktree_new(const std::string& arg) {
    if (busy_reject("worktree"))
        return;
    std::string name;
    std::string ref;
    if (!parse_worktree_new_arg(arg, name, ref))
        return;
    const std::string project = agent::Workspace::project_root();
    if (!agent::is_git_repo(project)) {
        tui_.append_line(P_STATUS, "worktree: " + project + " is not a git repository");
        return;
    }
    const std::string base = agent::git_resolve_commit(project, ref);
    if (base.empty()) {
        tui_.append_line(P_STATUS, "worktree: unknown ref: " + ref);
        return;
    }
    Window& w = tui_.win();
    if (!resolve_worktree_name(w, name, name))
        return;
    const std::string path = agent::worktree_base_dir(project) + "/" + name;
    if (!create_worktree_record(name, path, base))
        return;
    const auto entries = agent::load_worktrees(agent::Workspace::state_dir());
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const agent::WorktreeEntry& e) { return e.name == name; });
    if (found == entries.end()) {
        tui_.append_line(P_STATUS,
                         "worktree: created " + path + " but its registry row is missing");
        return;
    }
    // Only now, with the new directory safely on disk, release the window from
    // its previous workspace and claim the new one.
    if (!attach_workspace_to(w, *found, tui_.worktree_claim(w)))
        return;
    tui_.append_line(P_STATUS, "worktree: created " + name + " at " + path + " from " + ref +
                                   " - detached; create a branch before the first commit");
    refresh_worktree_feed();
    tui_.draw();
}

// Whether the active window may claim this workspace. Everything that can
// refuse is here, so the handler below only has to act.
bool SlashDispatcher::workspace_attachable(const agent::WorktreeEntry& entry,
                                           const agent::WorktreeStatus& status, const Window& w,
                                           const std::string& claim) {
    if (!status.exists) {
        tui_.append_line(P_STATUS,
                         "worktree: " + entry.name + " directory is gone (try /worktree prune)");
        return false;
    }
    if (status.live && entry.session_id != claim) {
        tui_.append_line(P_STATUS, "worktree: " + entry.name + " is in use by another window (" +
                                       status.owner + ")");
        return false;
    }
    if (w.worktree == entry.name) {
        tui_.append_line(P_STATUS,
                         "worktree: " + entry.name + " is already attached to this window");
        return false;
    }
    return true;
}

bool SlashDispatcher::attach_workspace_to(Window& w, const agent::WorktreeEntry& entry,
                                          const std::string& claim) {
    if (!w.worktree.empty()) {
        const std::string previous = w.worktree;
        tui_.detach_window_from_workspace(w, /*park=*/true);
        tui_.append_line(P_STATUS, "worktree: parked " + previous);
    }
    std::string err;
    if (!agent::claim_worktree(agent::Workspace::state_dir(), entry.name, claim,
                               tui_.live_worktree_claims(), err)) {
        tui_.append_line(P_STATUS, "worktree: could not attach " + entry.name + " - " + err);
        return false;
    }
    w.root = entry.path;
    w.worktree = entry.name;
    w.base_oid = entry.base_oid;
    if (w.agent)
        w.agent->set_workspace_root(entry.path);
    tui_.session_controller_->save_window_sessions();
    tui_.session_controller_->save_workspace_now();
    return true;
}

// /worktree attach <name>: claim an existing workspace for this window.
void SlashDispatcher::cmd_worktree_attach(const std::string& name) {
    if (busy_reject("worktree"))
        return;
    const std::string state = agent::Workspace::state_dir();
    const std::string project = agent::Workspace::project_root();
    const auto entries = agent::load_worktrees(state);
    if (name.empty()) {
        std::string names;
        for (const auto& e : entries)
            names += (names.empty() ? "" : " ") + e.name;
        tui_.append_line(P_STATUS, "usage: /worktree attach <name>" +
                                       (names.empty() ? std::string(" - no workspaces")
                                                      : " - available: " + names));
        return;
    }
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const agent::WorktreeEntry& e) { return e.name == name; });
    if (found == entries.end()) {
        tui_.append_line(P_STATUS,
                         "worktree: no workspace named " + name + " (try /worktree list)");
        return;
    }
    Window& w = tui_.win();
    const std::string claim = tui_.worktree_claim(w);
    const agent::WorktreeStatus status = agent::worktree_status(
        project, *found, tui_.live_worktree_claims(), tui_.live_worktree_paths());
    if (!workspace_attachable(*found, status, w, claim))
        return;
    if (!attach_workspace_to(w, *found, claim))
        return;
    tui_.append_line(P_STATUS, "worktree: attached " + name + " at " + found->path + " (" +
                                   status.branch + ")");
    refresh_worktree_feed();
    tui_.draw();
}

void SlashDispatcher::cmd_worktree_detach() {
    if (busy_reject("worktree"))
        return;
    Window& w = tui_.win();
    if (w.worktree.empty()) {
        tui_.append_line(P_STATUS, "worktree: this window has none (it works in the project root)");
        return;
    }
    const std::string name = w.worktree;
    tui_.detach_window_from_workspace(w, /*park=*/true);
    tui_.append_line(P_STATUS, "worktree: detached; " + name +
                                   " parked (attach it again with /worktree attach " + name + ")");
    refresh_worktree_feed();
    tui_.draw();
}

// /worktree remove <name>: remove an unclaimed workspace when nothing is lost.
void SlashDispatcher::cmd_worktree_remove(const std::string& name) {
    const std::string state = agent::Workspace::state_dir();
    const std::string project = agent::Workspace::project_root();
    const auto entries = agent::load_worktrees(state);
    if (name.empty()) {
        std::string names;
        for (const auto& e : entries)
            names += (names.empty() ? "" : " ") + e.name;
        tui_.append_line(P_STATUS, "usage: /worktree remove <name>" +
                                       (names.empty() ? std::string(" - no workspaces")
                                                      : " - available: " + names));
        return;
    }
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const agent::WorktreeEntry& e) { return e.name == name; });
    if (found == entries.end()) {
        tui_.append_line(P_STATUS, "worktree: no workspace named " + name);
        return;
    }
    const agent::WorktreeStatus status = agent::worktree_status(
        project, *found, tui_.live_worktree_claims(), tui_.live_worktree_paths());
    if (status.live) {
        tui_.append_line(P_STATUS, "worktree: " + name +
                                       " is in use; leave it from its window first "
                                       "(/worktree detach or /set worktree off)");
        return;
    }
    tui_.append_line(P_STATUS, agent::release_workspace(project, state, found->name, found->path));
    refresh_worktree_feed();
    tui_.draw();
}

void SlashDispatcher::cmd_worktree_status() {
    Window& w = tui_.win();
    const std::string project = agent::Workspace::project_root();
    if (w.worktree.empty() && w.root.empty()) {
        tui_.append_line(P_STATUS, "worktree: none - this window works in " + project);
        return;
    }
    const auto entries = agent::load_worktrees(agent::Workspace::state_dir());
    const auto found =
        std::find_if(entries.begin(), entries.end(), [&](const agent::WorktreeEntry& e) {
            return (!w.worktree.empty() && e.name == w.worktree) ||
                   (!w.root.empty() && e.path == w.root);
        });
    if (found == entries.end()) {
        tui_.append_line(P_STATUS, "worktree: " + (w.worktree.empty() ? w.root : w.worktree) +
                                       " at " + w.root + " (not in the registry)");
        return;
    }
    const agent::WorktreeStatus status = agent::worktree_status(
        project, *found, tui_.live_worktree_claims(), tui_.live_worktree_paths());
    tui_.append_line(P_STATUS,
                     "worktree: " + status.name + " (" + worktree_state_label(status.state) + ")");
    tui_.append_line(P_STATUS, "  path: " + status.path);
    std::string branch = status.branch;
    if (!status.head_oid.empty())
        branch += " (" + status.head_oid.substr(0, 12) + ")";
    tui_.append_line(P_STATUS, "  branch: " + branch);
    tui_.append_line(P_STATUS, std::string("  tree: ") + (status.dirty ? status.problem : "clean"));
    tui_.append_line(P_STATUS,
                     std::string("  commits: ") +
                         (status.reachable ? "reachable from a ref" : "reachable from no ref"));
    if (!status.owner.empty())
        tui_.append_line(P_STATUS, "  owner: " + status.owner);
}

void SlashDispatcher::cmd_worktree_list() {
    const std::string project = agent::Workspace::project_root();
    const auto entries = agent::load_worktrees(agent::Workspace::state_dir());
    if (entries.empty()) {
        tui_.append_line(P_STATUS, "worktree: none in this project");
        return;
    }
    const auto statuses = agent::worktree_statuses(project, entries, tui_.live_worktree_claims(),
                                                   tui_.live_worktree_paths());
    for (const auto& s : statuses) {
        std::string line = "  " + s.name + "  " + worktree_state_label(s.state) + "  " + s.branch;
        if (!s.head_oid.empty())
            line += " (" + s.head_oid.substr(0, 8) + ")";
        line += "  " + s.path;
        if (!s.problem.empty())
            line += "  - " + s.problem;
        else if (!s.owner.empty())
            line += "  - " + s.owner;
        tui_.append_line(P_STATUS, line);
    }
}

void SlashDispatcher::cmd_worktree_prune() {
    const std::string project = agent::Workspace::project_root();
    const auto removed = agent::prune_registry(project, agent::Workspace::state_dir());
    if (removed.empty()) {
        tui_.append_line(P_STATUS, "worktree: nothing to prune");
        return;
    }
    std::string names;
    for (const auto& name : removed)
        names += (names.empty() ? "" : " ") + name;
    tui_.append_line(P_STATUS, "worktree: dropped registry entries: " + names);
    refresh_worktree_feed();
    tui_.draw();
}

void SlashDispatcher::cmd_worktree_main() {
    if (tui_.win().worktree.empty()) {
        tui_.append_line(P_STATUS, "worktree: this window is already at the project root");
        return;
    }
    tui_.leave_window_workspace(tui_.win());
}

void SlashDispatcher::cmd_worktree(const std::string& arg) {
    if (!arg.empty())
        tui_.append_line(P_STATUS,
                         "usage: /worktree new [name] [at <ref>] | attach <name> | detach | "
                         "remove <name> | show | list | prune | main");
    cmd_worktree_list();
}

void SlashDispatcher::cmd_window_new(bool noworkdir, const std::string& name) {
    tui_.new_window("chat", /*prepare_workspace=*/!noworkdir, name);
    tui_.draw();
}
void SlashDispatcher::cmd_window_close() {
    tui_.close_window();
}
// /session reset: drop the conversation and every piece of per-turn state the
// window cached about it. Extracted from register_core_session_actions, where the
// inline lambda made the action list unreadable at 40 NLOC -- exactly at the cap.
void SlashDispatcher::cmd_session_reset() {
    if (tui_.win().agent) {
        tui_.win().agent->set_context({});
        tui_.win().agent->policy().clear_session();
    }
    tui_.win().stream_buf.clear();
    tui_.win().stream_ts.clear();
    tui_.win().reason.begin();
    tui_.win().scroll_top = 0;
    tui_.win().ctx_used.store(-1);
    tui_.win().ctx_estimate = 0;
    tui_.win().live_ctx_offset = 0;
    tui_.append_line(P_STATUS, "conversation cleared \u2014 next message starts fresh");
    tui_.render_engine_->set_drawer_open(false);
    tui_.draw();
}

void SlashDispatcher::cmd_window_list() {
    std::string s = "windows:";
    for (size_t i = 0; i < tui_.window_manager_->all().size(); ++i)
        s += " " + std::to_string(i + 1) + ":" + tui_.window_manager_->all()[i]->title +
             (i == tui_.window_manager_->active() ? "*" : "");
    tui_.append_line(P_STATUS, s);
}
void SlashDispatcher::cmd_window_rename(const std::string& name) {
    if (name.empty()) {
        tui_.append_line(P_STATUS, "usage: /window rename <name>");
        return;
    }
    tui_.win().title = name;
    tui_.append_line(P_STATUS, "renamed window to " + tui_.win().title);
    tui_.draw();
}
void SlashDispatcher::cmd_window_set(const std::string& arg) {
    if (arg.empty()) {
        tui_.append_line(P_STATUS, "usage: /window set <number>");
        return;
    }
    try {
        auto n = static_cast<size_t>(std::stoul(arg));
        auto r = tui_.window_ops_->set_window(n);
        if (r.ok)
            tui_.append_line(P_STATUS, "switched to window " + std::to_string(n));
        else
            tui_.append_line(P_STATUS, r.msg);
    } catch (const std::exception&) {
        tui_.append_line(P_STATUS, "usage: /window set <number> (got: " + arg + ")");
    }
}

namespace {
const char* job_state_name(agent::JobState s) {
    switch (s) {
    case agent::JobState::Starting:
        return "starting";
    case agent::JobState::Running:
        return "running";
    case agent::JobState::Done:
        return "done";
    case agent::JobState::Killed:
        return "killed";
    case agent::JobState::Failed:
        return "failed";
    }
    return "?";
}
std::string job_countdown(const agent::JobInfo& i) {
    long rem = i.remaining_hard_s;
    if (i.remaining_idle_s >= 0 && (rem < 0 || i.remaining_idle_s < rem))
        rem = i.remaining_idle_s;
    if (rem < 0)
        return "";
    return " ~" + std::to_string(rem) + "s";
}
std::string job_list_line(const agent::JobInfo& j) {
    return "id " + j.id + "  " + job_state_name(j.state) + "  pid " + std::to_string(j.pid) +
           "  age " + std::to_string(j.seconds_since_start) + "s  idle " +
           std::to_string(j.seconds_since_output) + "s" + job_countdown(j) + "  " + j.command;
}
} // namespace

void SlashDispatcher::cmd_compress(const std::string&) {
    auto& w = tui_.win();
    if (!w.agent) {
        tui_.append_line(P_STATUS, "no active session to compress");
        return;
    }
    if (tui_.runs_.busy(w.id)) {
        tui_.append_line(P_STATUS, "compress: agent is busy");
        return;
    }
    tui_.append_line(P_STATUS, "compressing...");
    w.state = agent::RunState::Waiting;
    Window* my_win = &w;
    size_t my_id = w.id;
    tui_.compress_worker(*my_win, my_id);
}

void SlashDispatcher::cmd_job(const std::string& rest) {
    if (!rest.empty())
        tui_.append_line(P_STATUS, "usage: /job [list|stop <id>|read <id>|start <cmd>]");
    job_ls();
}

void SlashDispatcher::job_ls() {
    auto jobs = tui_.jobs_.list();
    if (jobs.empty()) {
        tui_.append_line(P_STATUS, "no background jobs");
        return;
    }
    for (const auto& j : jobs)
        tui_.append_line(P_STATUS, job_list_line(j));
}

void SlashDispatcher::refresh_job_feed() {
    if (tui_.feed_manager_)
        tui_.feed_manager_->refresh_job_feed();
}

void SlashDispatcher::job_kill(const std::string& id) {
    if (id.empty()) {
        tui_.append_line(P_STATUS, "usage: /job stop <id>");
        return;
    }
    bool ok = tui_.jobs_.stop(id);
    tui_.append_line(P_STATUS, ok ? ("stopped " + id) : ("no such job: " + id));
    refresh_job_feed();
    tui_.draw();
}

void SlashDispatcher::job_read(const std::string& id) {
    if (id.empty()) {
        tui_.append_line(P_STATUS, "usage: /job read <id>");
        return;
    }
    std::string out = tui_.jobs_.output(id);
    if (out.empty()) {
        tui_.append_line(P_STATUS, "no output for " + id);
        return;
    }
    rich::Line body;
    rich::Run run;
    run.text = out;
    run.pair = P_STATUS;
    body.runs.push_back(run);
    for (auto& l : rich::wrap(body, tui_.render_engine_->width()))
        tui_.append_rich(l);
}

void SlashDispatcher::job_start(const std::string& cmd) {
    if (cmd.empty()) {
        tui_.append_line(P_STATUS, "usage: /job start <command>");
        return;
    }
    std::string id = tui_.jobs_.start(cmd, agent::Workspace::project_root(), 600, 30,
                                      agent::JobOrigin{tui_.win().owner_id, cmd});
    if (id.empty()) {
        tui_.append_line(P_STATUS, "failed to start: " + cmd);
        return;
    }
    tui_.append_line(P_STATUS, "started " + id + ": " + cmd);
    refresh_job_feed();
    tui_.draw();
}

void Tui::config_screen() const {
    auto mask = [](const std::string& s) {
        return s.empty() ? std::string("(unset)") : std::string(s.size(), '*');
    };
    const auto acfg = win().agent ? win().agent->config_snapshot() : nullptr;
    const int ctx = acfg ? acfg->context_size : cfg_.context_size;
    const bool ctx_explicit = acfg ? acfg->context_explicit : cfg_.context_explicit;
    info_dialog("Configuration",
                {
                    "api_base:  " + cfg_.api_base,
                    "api_key:   " + mask(cfg_.api_key),
                    "model:     " + (acfg ? acfg->model : cfg_.model),
                    "stream:    " + std::string(cfg_.stream ? "on" : "off"),
                    "context:   " + (ctx > 0 ? std::to_string(ctx) + " tokens" +
                                                   (ctx_explicit ? "" : " (auto-detected)")
                                             : std::string("auto (not detected)")),
                    "max_iter:  " + std::to_string(cfg_.max_tool_iterations),
                    "system:    " + cfg_.system_prompt_path,
                    "tools:     " + cfg_.tools_prompt_path,
                });
}

void Tui::detect_server(bool force) {
    // Cache-only on the UI thread (stale-while-revalidate): the disk catalog
    // answers immediately and a stale or missing entry revalidates on a
    // detached worker. The status line and model feed update when it lands.
    agent::ServerInfo info = agent::apply_cached_server_autodetect(cfg_);
    bool stale = true;
    if (auto e = agent::model_catalog_read(cfg_))
        stale = !agent::model_catalog_fresh(*e);
    if (info.ok) {
        last_detected_ = info;
        std::string note =
            "detected model=" + cfg_.model + " n_ctx=" + std::to_string(cfg_.context_size);
        if (info.context_train > 0 && info.context_train != cfg_.context_size)
            note += " (max " + std::to_string(info.context_train) + ")";
        append_line(P_STATUS, note);
        draw();
    }
    if (force || stale)
        refresh_models_async(force);
}

void Tui::refresh_provider_catalogs_async() {
    if (!providers_)
        return;
    for (const auto& p : providers_->available()) {
        // Only providers the USER configured, never a bundled preset.
        //
        // "has an api_base" is not the test: every preset ships one, so that is
        // true for all of them and startup would fire an unauthenticated /models
        // request per bundled endpoint. Presets set builtin=true (extensions.cpp
        // :523) and carry no api_key; a file-configured provider does not.
        if (p.builtin || p.api_base.empty())
            continue;
        agent::Config cfg;
        cfg.provider_name = p.name;
        cfg.api_base = p.api_base;
        cfg.api_key = p.api_key;
        cfg.flavor = p.flavor;
        auto entry = agent::model_catalog_read(cfg);
        if (entry && agent::model_catalog_fresh(*entry))
            continue; // already good
        agent::model_catalog_refresh_async(cfg, ui_poster(),
                                           [this](const agent::CatalogFetchReport&) {
                                               // Any catalogue landing changes the union, whichever
                                               // provider it came from. cfg_ is untouched: this
                                               // path exists to grow the list, not to switch or
                                               // autodetect the connection. A failure is reported
                                               // by the endpoint's own command, not from a warm-up.
                                               slash_dispatcher_->refresh_model_list();
                                               draw();
                                           });
    }
}

// /get provider list: one row per provider, printed at once, and a live GET
// /models per configured provider that settles its own row in place. Nothing is
// read from a cache: the answer is fetched when the user asks, so a row can
// never show a verdict older than the token it describes.
void Tui::show_provider_list() {
    if (!providers_)
        return;
    const unsigned request = ++provider_list_seq_;
    const std::string ts = timestamp();
    for (const auto& p : providers_->available()) {
        const bool active = p.name == cfg_.provider_name;
        // The active provider's key may live in the global config rather than in
        // its own definition (apply_selection keeps whatever is configured when
        // the definition carries none).
        const bool has_key = !p.api_key.empty() || (active && !cfg_.api_key.empty());

        ProviderListRow row;
        row.name = p.name;
        row.api_base = p.api_base;
        row.requires_key = p.requires_key;
        row.has_key = has_key;
        row.active = active;
        // Only a provider that can be queried at all is probed: no endpoint, or
        // a required key that is absent, is already a settled answer.
        row.pending = !p.api_base.empty() && (!p.requires_key || has_key);

        const size_t index = append_rich_to(win(), provider_list_line(row, ts));
        if (!row.pending)
            continue;

        provider_rows_.add(win().id, index, request, ts, row);
        agent::Config probe;
        probe.provider_name = p.name;
        probe.api_base = p.api_base;
        probe.flavor = p.flavor;
        probe.api_key = p.api_key.empty() && active ? cfg_.api_key : p.api_key;
        agent::auth_probe_async(probe, ui_poster(),
                                [this, request, name = p.name](const agent::AuthStatus& s) {
                                    settle_provider_row(request, name, s.state);
                                });
    }
    draw();
}

void Tui::settle_provider_row(unsigned request, const std::string& name, agent::AuthState state) {
    auto settled = provider_rows_.settle(request, name, state);
    if (!settled)
        return; // trimmed away, or its window's scrollback was replaced
    if (Window* w = window_by_id(settled->window_id); w && settled->index < w->lines.size())
        w->lines[settled->index] = std::move(settled->line);
    draw();
}

void Tui::refresh_models_async(bool announce) {
    if (models_refresh_inflight_.exchange(true))
        return;
    const std::string api_base = cfg_.api_base;
    const std::string flavor = cfg_.flavor;
    agent::model_catalog_refresh_async(
        cfg_, ui_poster(), [this, announce, api_base, flavor](const agent::CatalogFetchReport& r) {
            on_models_refreshed(r, announce, api_base, flavor);
        });
}

void Tui::on_models_refreshed(const agent::CatalogFetchReport& refresh, bool announce,
                              const std::string& api_base, const std::string& flavor) {
    models_refresh_inflight_ = false;
    // /get model list asked for an answer; consume the ask here whatever the
    // endpoint, so a failure for a switched-away endpoint cannot surface later.
    const bool asked = std::exchange(report_model_failure_, false);
    // A provider switch while the fetch was in flight makes the result belong
    // to a different endpoint; drop it rather than merge the wrong catalog.
    if (cfg_.api_base != api_base || cfg_.flavor != flavor)
        return;
    // Say WHY, not just that the list is missing: the endpoint asked and the
    // status that came back are the two facts a wrong server URL (or the wrong
    // protocol) shows up in, and the user who asked is the one who can act.
    if (!refresh.fetched && (announce || asked))
        append_line(P_STATUS, refresh.failure_reason());
    agent::ServerInfo info = agent::apply_cached_server_autodetect(cfg_);
    if (info.ok) {
        const bool changed =
            info.model != last_detected_.model || info.context_size != last_detected_.context_size;
        last_detected_ = info;
        if (announce || changed) {
            std::string note =
                "detected model=" + cfg_.model + " n_ctx=" + std::to_string(cfg_.context_size);
            if (info.context_train > 0 && info.context_train != cfg_.context_size)
                note += " (max " + std::to_string(info.context_train) + ")";
            append_line(P_STATUS, note);
        }
        // Windows opened before the catalog resolved still hold an empty
        // model; adopt the detected one now — but only for idle windows: while a
        // run or compression is in flight the agent owns its config (single
        // ownership, not locking). A busy window with no model resolves it in
        // Agent::run() against this now-warm catalog.
        adopt_detected_model();
        apply_detected_context_window();
        refresh_model_list();
    }
    draw();
}

void Tui::adopt_detected_model() {
    for (auto& w : window_manager_->all()) {
        if (!w->agent || runs_.busy(w->id))
            continue;
        const auto acfg = w->agent->config_snapshot();
        if (!acfg || acfg->model.empty())
            w->agent->set_model(cfg_.model);
    }
}

// The catalogue's window belongs to the model it describes. Push it into the
// idle agents on that model: the gauge and compression read the agent
// snapshot, so without this a provider switch leaves them on the previous
// endpoint's value until the next restart. A user pin is ignored by
// set_context_window.
void Tui::apply_detected_context_window() {
    for (auto& w : window_manager_->all()) {
        if (!w->agent || runs_.busy(w->id))
            continue;
        const auto acfg = w->agent->config_snapshot();
        if (acfg && acfg->model == cfg_.model) {
            w->agent->set_context_window(cfg_.context_size);
            w->agent->set_model_capabilities(agent::capabilities_of(cfg_));
        }
    }
}

void Tui::test_connection(bool announce) {
    append_line(P_STATUS, "testing " + cfg_.api_base + "...");
    const std::string api_base = cfg_.api_base;
    const std::string flavor = cfg_.flavor;
    agent::model_catalog_refresh_async(
        cfg_, ui_poster(), [this, api_base, flavor](const agent::CatalogFetchReport& r) {
            if (cfg_.api_base != api_base || cfg_.flavor != flavor)
                return;
            if (!r.fetched) {
                append_line(P_STATUS, "test: " + r.failure_reason());
                draw();
                return;
            }
            agent::ServerInfo info = agent::apply_cached_server_autodetect(cfg_);
            if (!info.ok) {
                append_line(P_STATUS, "test: " + api_base + " answered but listed no models");
                draw();
                return;
            }
            last_detected_ = info;
            std::string note = "test: OK - " + cfg_.api_base + "  model=" + cfg_.model +
                               " n_ctx=" + std::to_string(cfg_.context_size);
            if (info.context_train > 0 && info.context_train != cfg_.context_size)
                note += " (max " + std::to_string(info.context_train) + ")";
            append_line(P_STATUS, note);
            refresh_model_list();
            draw();
        });
    (void)announce;
}

static bool edit_provider_form(agent::Config& cfg, const std::string& title) {
    std::string model_field = cfg.model_explicit ? cfg.model : "";
    std::vector<FieldSpec> fields = {
        {"Server URL", cfg.api_base, false},
        {"API Key", cfg.api_key, true},
        {"Model (blank = auto)", model_field, false},
    };
    if (!form_edit(title, fields))
        return false;
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
    return true;
}

std::string Tui::prompt_api_key(const std::string& reason) {
    // Runs on the UI thread (EventRouter::resolve_api_key). Seed the form
    // from the active provider's current values so only the key needs
    // typing; Esc/Cancel returns "" (the agent then degrades gracefully).
    const std::string provider = cfg_.provider_name.empty() ? "custom" : cfg_.provider_name;
    agent::Config prov_cfg;
    prov_cfg.provider_name = provider;
    prov_cfg.api_base = cfg_.api_base;
    prov_cfg.api_key = cfg_.api_key;
    prov_cfg.model = cfg_.model;
    prov_cfg.model_explicit = cfg_.model_explicit;
    prov_cfg.context_size = cfg_.context_size;
    prov_cfg.context_explicit = cfg_.context_explicit;

    std::vector<FieldSpec> fields = {
        {"Provider", provider, false},
        {"API Key", prov_cfg.api_key, true},
    };
    if (!form_edit("API key required", fields))
        return "";
    std::string key = fields[1].value;
    if (key.empty())
        return ""; // blank = cancel
    prov_cfg.api_key = key;

    // Persist to the provider's own config file (overlays the preset on
    // restart) and to the global config, exactly like the provider editor.
    auto sel = providers_->find(provider);
    agent::Provider existing = sel ? *sel : agent::Provider{};
    providers_->save(agent::provider_from_edit(existing, prov_cfg, existing.builtin));
    cfg_.api_key = key;
    cfg_.provider_name = provider;
    cfg_.save_global(agent::global_config_path());
    (void)reason;
    return key;
}

// Build the provider picker rows: one per provider (the active one marked, with
// its key hint) plus a trailing "Add new provider..." sentinel. `ids` is the
// parallel id list; its last entry is "" for the sentinel.
void Tui::build_provider_picker(std::vector<std::string>& rows,
                                std::vector<std::string>& ids) const {
    const auto providers = providers_->available();

    std::vector<std::string> prov_display;
    for (const auto& p : providers) {
        std::string label =
            p.name + "  (" + (p.api_base.empty() ? "unconfigured" : p.api_base) + ")";
        prov_display.push_back((cfg_.provider_name == p.name ? "> " : "  ") + label);
        ids.push_back(p.name);
    }
    // Only the active provider's key tells us anything about the others, so the
    // hint is reported for it alone.
    rows.clear();
    for (size_t i = 0; i < ids.size(); ++i) {
        const std::string& id = ids[i];
        if (id.empty()) {
            rows.push_back(prov_display[i]);
            continue;
        }
        const bool active = (id == cfg_.provider_name);
        const char* key_hint = (active && cfg_.api_key.empty()) ? "no-key" : "key-set";
        std::string line;
        line.reserve(id.size() + 12);
        line.append(active ? "> " : "  ").append(id);
        line.append("  (").append(key_hint).push_back(')');
        rows.push_back(std::move(line));
    }
    rows.emplace_back("  + Add new provider...");
    ids.emplace_back(""); // sentinel
}

void Tui::add_new_provider() {
    // Ask for provider name
    std::vector<FieldSpec> name_field = {{"Provider name", "", false}};
    if (!form_edit("New Provider", name_field))
        return;
    std::string new_name = name_field[0].value;
    if (new_name.empty())
        return;

    // Seed from the provider domain (built-in or saved).
    agent::Config prov_cfg;
    agent::Provider existing;
    prov_cfg.provider_name = new_name;
    if (auto p = providers_->find(new_name)) {
        existing = *p;
        prov_cfg.api_base = p->api_base;
        prov_cfg.api_key = p->api_key;
        prov_cfg.model = p->default_model;
    }
    if (!edit_provider_form(prov_cfg, "Edit: " + new_name))
        return;
    prov_cfg.provider_name = new_name;
    providers_->save(agent::provider_from_edit(existing, prov_cfg, false));
    // Adopt through the domain rather than assigning cfg_ here: the saved
    // definition is the single source of the protocol, the key and the model,
    // and the switch has to reach the window agent too.
    if (!slash_dispatcher_->adopt_provider(new_name))
        return;
    refresh_provider_feed();
    append_line(P_STATUS, "provider '" + new_name + "' added and activated");
}

// Activate & edit: seed from the provider domain, overlay current values when it
// is already the active provider, then confirm through the shared edit form.
void Tui::activate_and_edit_provider(const std::string& id, const agent::Provider* sel,
                                     bool is_preset) {
    agent::Config prov_cfg;
    agent::Provider existing = sel ? *sel : agent::Provider{};
    prov_cfg.provider_name = id;
    if (sel) {
        prov_cfg.api_base = sel->api_base;
        prov_cfg.api_key = sel->api_key;
        prov_cfg.model = sel->default_model;
    }
    if (cfg_.provider_name == id) {
        prov_cfg.api_base = cfg_.api_base;
        prov_cfg.api_key = cfg_.api_key;
        prov_cfg.model = cfg_.model;
    }
    if (!edit_provider_form(prov_cfg, "Edit: " + id))
        return;

    providers_->save(agent::provider_from_edit(existing, prov_cfg, is_preset));
    // Re-resolve through the provider domain: the form edits the definition,
    // the domain decides what the session speaks. This is what carries the
    // protocol (and the endpoint/key/model) to cfg_, the window agent and the
    // feeds in one step.
    if (!slash_dispatcher_->adopt_provider(id))
        return;
    refresh_provider_feed();
    append_line(P_STATUS, "provider '" + id + "' activated");
    test_connection(false);
}

void Tui::provider_actions(const std::string& selected_id) {
    const auto sel_provider = providers_->find(selected_id);
    const bool is_preset = sel_provider && sel_provider->builtin;
    std::vector<std::string> actions = {"Activate & edit", "Test connection"};
    if (!is_preset)
        actions.emplace_back("Delete provider");
    const int action = menu_select("Provider: " + selected_id, actions);
    if (action < 0)
        return;

    if (action == 0) {
        activate_and_edit_provider(selected_id, sel_provider ? &*sel_provider : nullptr, is_preset);
    } else if (action == 1) {
        // Test connection.
        cfg_.provider_name = selected_id;
        cfg_.save_global(agent::global_config_path());
        test_connection(true);
    } else if (action == 2 && !is_preset) {
        // Delete provider.
        std::string msg = "Delete provider \"" + selected_id + "\"?";
        tui::ConfirmPanel confirm("Delete Provider", msg);
        if (confirm.run()) {
            providers_->remove(selected_id);
            refresh_provider_feed();
            append_line(P_STATUS, "provider '" + selected_id + "' deleted");
        }
    }
}

void Tui::settings_screen() {
    // Step 1: build the provider picker (saved + built-in presets).
    std::vector<std::string> rows;
    std::vector<std::string> ids;
    build_provider_picker(rows, ids);
    const int add_new_idx = static_cast<int>(ids.size()) - 1; // the sentinel row

    // Step 2: select a provider, or the "Add new" action.
    ModalScope scope;
    curs_set(0);
    int sel;
    {
        ListPanel lp("Providers (" + std::to_string(ids.size() - 1) + " configured)", rows);
        sel = lp.run();
    }
    if (sel < 0)
        return;

    if (sel == add_new_idx) {
        add_new_provider();
        return;
    }

    const std::string selected_id = ids[sel];
    if (selected_id.empty())
        return;

    // Step 3: act on the selected provider.
    provider_actions(selected_id);
}

void SlashDispatcher::apply_compression_threshold(const std::string& v) {
    auto t = text::parse_setting_double(v, 0.1, 1.0);
    if (!t) {
        tui_.append_line(P_STATUS, "compression.threshold: invalid value (0.1-1.0): " + v);
        return;
    }
    tui_.cfg_.compression_threshold = *t;
    tui_.cfg_.compression_threshold_explicit = true;
    if (tui_.win().agent)
        tui_.win().agent->set_compression_threshold(*t);
    tui_.append_line(P_STATUS, "compression threshold: " + std::to_string(*t));
    if (!tui_.cfg_.save_settings(tui_.session_controller_->settings_path()))
        tui_.append_line(P_STATUS,
                         "warning: could not save to " + tui_.session_controller_->settings_path());
    tui_.draw();
}

void SlashDispatcher::apply_compression_min_turns(const std::string& v) {
    auto n = text::parse_setting_int(v, 0, 999);
    if (!n) {
        tui_.append_line(P_STATUS, "compression.min_turns: invalid value (0-999): " + v);
        return;
    }
    tui_.cfg_.compression_min_turns = *n;
    tui_.cfg_.compression_min_turns_explicit = true;
    if (tui_.win().agent)
        tui_.win().agent->set_compression_min_turns(*n);
    tui_.append_line(P_STATUS, "compression min_turns: " + std::to_string(*n));
    if (!tui_.cfg_.save_settings(tui_.session_controller_->settings_path()))
        tui_.append_line(P_STATUS,
                         "warning: could not save to " + tui_.session_controller_->settings_path());
    tui_.draw();
}

void SlashDispatcher::apply_compression_target_pct(const std::string& v) {
    auto n = text::parse_setting_int(v, 1, 90);
    if (!n) {
        tui_.append_line(P_STATUS, "compression.target_pct: invalid value (1-90): " + v);
        return;
    }
    tui_.cfg_.compression_target_pct = *n;
    tui_.cfg_.compression_target_pct_explicit = true;
    if (tui_.win().agent)
        tui_.win().agent->set_compression_target_pct(*n);
    tui_.append_line(P_STATUS, "compression target_pct: " + std::to_string(*n));
    if (!tui_.cfg_.save_settings(tui_.session_controller_->settings_path()))
        tui_.append_line(P_STATUS,
                         "warning: could not save to " + tui_.session_controller_->settings_path());
    tui_.draw();
}

void SlashDispatcher::apply_compression_keep_last_prompts(const std::string& v) {
    auto n = text::parse_setting_int(v, 1, 100);
    if (!n) {
        tui_.append_line(P_STATUS, "compression.keep_last_prompts: invalid value (1-100): " + v);
        return;
    }
    tui_.cfg_.compression_keep_last_prompts = *n;
    tui_.cfg_.compression_keep_last_prompts_explicit = true;
    if (tui_.win().agent)
        tui_.win().agent->set_compression_keep_last_prompts(*n);
    tui_.append_line(P_STATUS, "compression keep_last_prompts: " + std::to_string(*n));
    if (!tui_.cfg_.save_settings(tui_.session_controller_->settings_path()))
        tui_.append_line(P_STATUS,
                         "warning: could not save to " + tui_.session_controller_->settings_path());
    tui_.draw();
}

void SlashDispatcher::add_setting(const std::string& key, std::function<std::string()> getter,
                                  std::function<void(const std::string&)> setter) {
    tui_.settings_.add({key, std::move(getter), std::move(setter)});
}

void SlashDispatcher::build_settings() {
    tui_.settings_ = tui::SettingRegistry{};
    add_detection_settings();
    add_debug_settings();
    add_reasoning_settings();
    add_subagent_settings();
    add_work_settings();
    add_display_settings();
    add_policy_settings();
    add_think_settings();
    add_compression_settings();
}

void SlashDispatcher::add_work_settings() {
    add_setting(
        "work.auto_attach", [this]() { return tui_.cfg_.work_auto_attach; },
        [this](const std::string& v) { cmd_set_work_auto_attach(v); });
}

void SlashDispatcher::cmd_set_work_auto_attach(const std::string& v) {
    if (v != "off" && v != "agents" && v != "jobs" && v != "all")
        throw std::invalid_argument("expected off|agents|jobs|all");
    tui_.cfg_.work_auto_attach = v;
}

void SlashDispatcher::add_detection_settings() {
    add_setting(
        "detection.loop", [this]() { return tui_.cfg_.detection_loop ? "on" : "off"; },
        [this](const std::string& v) {
            tui_.cfg_.detection_loop = parse_toggle(v, tui_.cfg_.detection_loop);
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
            if (tui_.win().agent)
                tui_.win().agent->set_detection_loop(tui_.cfg_.detection_loop);
        });
    add_setting(
        "detection.duplicate", [this]() { return tui_.cfg_.detection_duplicate ? "on" : "off"; },
        [this](const std::string& v) {
            tui_.cfg_.detection_duplicate = parse_toggle(v, tui_.cfg_.detection_duplicate);
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
        });
}

// Diagnostics are opt-in: with the switch off nothing is written and every
// diagnostic path costs one bool test. Turning it on mid-session opens the
// journal for the running agent, so the next failure is recorded.
void SlashDispatcher::set_debug(const std::string& val) {
    tui_.cfg_.debug = parse_toggle(val, tui_.cfg_.debug);
    tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
    if (tui_.cfg_.debug && tui_.win().agent)
        tui_.win().agent->log().open_errors(agent::error_log_path());
    show_debug_status();
}

void SlashDispatcher::show_debug_status() {
    tui_.append_line(P_STATUS, "diagnostics: " + std::string(tui_.cfg_.debug ? "on" : "off") +
                                   "  —  " + agent::error_log_path());
}

void SlashDispatcher::add_debug_settings() {
    add_setting(
        "debug", [this]() { return tui_.cfg_.debug ? "on" : "off"; },
        [this](const std::string& v) { set_debug(v); });
}

void SlashDispatcher::add_reasoning_settings() {
    add_setting(
        "reasoning.effort", [this]() { return tui_.cfg_.reasoning_effort; },
        [this](const std::string& v) {
            if (v != "off" && v != "low" && v != "medium" && v != "high")
                return;
            if (busy_reject("reasoning effort"))
                return;
            tui_.cfg_.reasoning_effort = v;
            if (tui_.win().agent)
                tui_.win().agent->set_reasoning_effort(v);
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
        });
}

void SlashDispatcher::add_subagent_settings() {
    add_setting(
        "subagent.parallel", [this]() { return tui_.subagents_.parallel() ? "on" : "off"; },
        [this](const std::string& v) {
            tui_.subagents_.set_parallel(parse_toggle(v, tui_.subagents_.parallel()));
            tui_.cfg_.subagent_parallel = tui_.subagents_.parallel();
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
        });
    add_setting(
        "subagent.max", [this]() { return std::to_string(tui_.subagents_.max()); },
        [this](const std::string& v) {
            auto n = text::parse_setting_int(v, 1, 16);
            if (!n) {
                tui_.append_line(P_STATUS, "subagent.max: invalid value (1-16): " + v);
                return;
            }
            tui_.subagents_.set_max(*n);
            tui_.cfg_.subagent_max = *n;
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
        });
}

void SlashDispatcher::add_display_settings() {
    add_setting(
        "display.markdown", [this]() { return tui_.win().markdown_on ? "on" : "off"; },
        [this](const std::string& v) {
            tui_.win().markdown_on = (v == "on");
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
        });
}

// Mode lives on the agent (the approval gate reads it live) — propagate to the
// ACTIVE window only; it never reached any agent before, so existing windows
// kept their launch mode.
void SlashDispatcher::apply_policy_mode(const std::string& v) {
    agent::AgentMode m = agent::AgentMode::Write;
    if (v == "read")
        m = agent::AgentMode::Read;
    else if (v == "yolo")
        m = agent::AgentMode::Yolo;
    tui_.cfg_.mode = m;
    if (tui_.win().agent)
        tui_.win().agent->set_mode(m);
}

void SlashDispatcher::add_policy_settings() {
    add_setting(
        "policy.mode",
        [this]() -> std::string {
            const auto& w = tui_.win();
            const auto acfg = w.agent ? w.agent->config_snapshot() : nullptr;
            return mode_name(acfg ? acfg->mode : tui_.cfg_.mode);
        },
        [this](const std::string& v) { apply_policy_mode(v); });
    add_setting(
        "policy.timeout", [this]() -> std::string { return std::to_string(tui_.policy_timeout_); },
        [this](const std::string& v) {
            auto n = text::parse_setting_int(v, 0, 999);
            if (!n) {
                tui_.append_line(P_STATUS, "policy.timeout: invalid value (0-999): " + v);
                return;
            }
            tui_.policy_timeout_ = *n;
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
        });
    add_setting(
        "policy.approval",
        [this]() -> std::string { return tui_.cfg_.policy_approval ? "on" : "off"; },
        [this](const std::string& v) {
            tui_.cfg_.policy_approval = parse_toggle(v, tui_.cfg_.policy_approval);
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
            tui_.append_line(P_STATUS, std::string("policy approval: ") +
                                           (tui_.cfg_.policy_approval ? "on" : "off"));
        });
    // Namespace root for /get policy (no setter — children handle values).
    add_setting("policy", []() -> std::string { return ""; }, nullptr);
}

void SlashDispatcher::add_think_settings() {
    add_setting(
        "think",
        [this]() -> std::string {
            const auto& w = tui_.win();
            const auto acfg = w.agent ? w.agent->config_snapshot() : nullptr;
            return acfg ? acfg->thinking : tui_.cfg_.thinking;
        },
        [this](const std::string& v) {
            tui_.cfg_.thinking = v;
            if (tui_.win().agent)
                tui_.win().agent->set_thinking(v);
            tui_.cfg_.save_settings(tui_.session_controller_->settings_path());
        });
}

void SlashDispatcher::add_compression_settings() {
    add_setting(
        "compression.threshold",
        [this]() -> std::string { return std::to_string(compression_threshold_effective()); },
        [this](const std::string& v) { apply_compression_threshold(v); });
    add_setting(
        "compression.min_turns",
        [this]() -> std::string {
            return std::to_string(
                tui_.cfg_.compression_min_turns > 0 ? tui_.cfg_.compression_min_turns : 10);
        },
        [this](const std::string& v) { apply_compression_min_turns(v); });
    add_setting(
        "compression.target_pct",
        [this]() -> std::string {
            return std::to_string(agent::load_compression_config(tui_.cfg_).target_pct);
        },
        [this](const std::string& v) { apply_compression_target_pct(v); });
    add_setting(
        "compression.keep_last_prompts",
        [this]() -> std::string {
            return std::to_string(agent::load_compression_config(tui_.cfg_).keep_last_prompts);
        },
        [this](const std::string& v) { apply_compression_keep_last_prompts(v); });
}

} // namespace tui

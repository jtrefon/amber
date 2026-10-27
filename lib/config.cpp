
#include "agent/config.h"
#include "agent/private_file.h"
#include "agent/workspace.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <unistd.h>

namespace agent {

namespace {
// Tolerant numeric parsing: one bad value skips that key (default kept) and
// is reported in Config::warnings — it must never abort startup or discard
// the whole config file.
bool parse_num(const std::string& key, const std::string& val, std::vector<std::string>& warnings,
               long& out) {
    try {
        out = std::stol(val);
        return true;
    } catch (const std::exception&) {
        warnings.push_back(key + "=\"" + val + "\" is not a number; using default");
        return false;
    }
}

void parse_int(const std::string& key, const std::string& val, std::vector<std::string>& warnings,
               int& out) {
    long n = 0;
    if (parse_num(key, val, warnings, n))
        out = static_cast<int>(n);
}

void parse_uint(const std::string& key, const std::string& val, std::vector<std::string>& warnings,
                size_t& out) {
    long n = 0;
    if (parse_num(key, val, warnings, n) && n >= 0)
        out = static_cast<size_t>(n);
}

void parse_double(const std::string& key, const std::string& val,
                  std::vector<std::string>& warnings, double& out) {
    try {
        out = std::stod(val);
    } catch (const std::exception&) {
        warnings.push_back(key + "=\"" + val + "\" is not a number; using default");
    }
}

bool parse_bool(const std::string& val) {
    return val == "1" || val == "true" || val == "yes";
}

// One KEY=VALUE line. Blanks, comments and lines without '=' yield an empty key,
// which the caller skips. Pure, so the line grammar is testable on its own.
struct Setting {
    std::string key;
    std::string value;
};

Setting split_setting(const std::string& line) {
    if (line.empty() || line[0] == '#')
        return {};
    auto eq = line.find('=');
    if (eq == std::string::npos)
        return {};
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    if (!value.empty() && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return {key, value};
}

// One applier per setting domain. Each owns its keys and stays deliberately
// dumb: the point of the split is that no single function has to know all 40.
void apply_connection_keys(Config& c, const std::string& key, const std::string& val) {
    if (key == "api_base")
        c.api_base = val;
    else if (key == "api_key")
        c.api_key = val;
    else if (key == "kilo_balance_token")
        c.kilo_balance_token = val;
    else if (key == "provider")
        c.provider_name = val;
    else if (key == "model") {
        // An empty model in the config means "auto-detect from the server";
        // do not treat it as an explicit choice (that would disable probing).
        c.model = val;
        c.model_explicit = !val.empty();
    }
}

void apply_prompt_keys(Config& c, const std::string& key, const std::string& val) {
    if (key == "system_prompt")
        c.system_prompt_path = val;
    else if (key == "tools_prompt")
        c.tools_prompt_path = val;
    else if (key == "git_prompt")
        c.git_prompt_path = val;
}

void apply_sampling_keys(Config& c, const std::string& key, const std::string& val,
                         std::vector<std::string>& warnings) {
    if (key == "temperature")
        parse_double(key, val, warnings, c.temperature);
    else if (key == "max_tokens")
        parse_uint(key, val, warnings, c.max_tokens);
    else if (key == "stream")
        c.stream = parse_bool(val);
    else if (key == "thinking")
        c.thinking = val;
    else if (key == "thinking_budget")
        parse_int(key, val, warnings, c.thinking_budget);
    else if (key == "reasoning_effort")
        c.reasoning_effort = val;
    else if (key == "show_reasoning")
        c.show_reasoning = parse_bool(val);
}

void apply_agent_keys(Config& c, const std::string& key, const std::string& val,
                      std::vector<std::string>& warnings) {
    if (key == "max_tool_iterations")
        parse_int(key, val, warnings, c.max_tool_iterations);
    else if (key == "subagent_parallel")
        c.subagent_parallel = parse_bool(val);
    else if (key == "subagent_max")
        parse_int(key, val, warnings, c.subagent_max);
    else if (key == "work_auto_attach") {
        if (val == "off" || val == "agents" || val == "jobs" || val == "all")
            c.work_auto_attach = val;
        else
            warnings.push_back("work_auto_attach: expected off|agents|jobs|all, got '" + val + "'");
    } else if (key == "max_tool_result_bytes")
        parse_int(key, val, warnings, c.max_tool_result_bytes);
    else if (key == "context_size") {
        // 0 (or negative) means "auto-detect"; only a positive value counts
        // as an explicit override that suppresses server probing.
        parse_int(key, val, warnings, c.context_size);
        c.context_explicit = c.context_size > 0;
    } else if (key == "default_context_size") {
        // Provider-level default; does NOT set context_explicit so
        // server auto-detect and user-override can still win.
        if (!val.empty())
            parse_int(key, val, warnings, c.default_context_size);
    } else if (key == "policy_approval")
        c.policy_approval = parse_bool(val);
}

void apply_detection_keys(Config& c, const std::string& key, const std::string& val) {
    if (key == "detection_loop")
        c.detection_loop = parse_bool(val);
    else if (key == "detection_duplicate")
        c.detection_duplicate = parse_bool(val);
    else if (key == "wallet")
        c.wallet_enabled = parse_bool(val);
    else if (key == "worktree")
        c.worktree_auto = parse_bool(val);
}

void apply_compression_keys(Config& c, const std::string& key, const std::string& val,
                            std::vector<std::string>& warnings) {
    if (key == "compression_threshold") {
        parse_double(key, val, warnings, c.compression_threshold);
        c.compression_threshold_explicit = true;
    } else if (key == "compression_min_turns") {
        parse_int(key, val, warnings, c.compression_min_turns);
        c.compression_min_turns_explicit = true;
    } else if (key == "compression_cooldown_turns") {
        parse_int(key, val, warnings, c.compression_cooldown_turns);
        c.compression_cooldown_turns_explicit = true;
    } else if (key == "compression_target_pct") {
        parse_int(key, val, warnings, c.compression_target_pct);
        c.compression_target_pct_explicit = true;
    } else if (key == "compression_keep_last_prompts") {
        parse_int(key, val, warnings, c.compression_keep_last_prompts);
        c.compression_keep_last_prompts_explicit = true;
    }
}

void apply_experience_keys(Config& c, const std::string& key, const std::string& val,
                           std::vector<std::string>& warnings) {
    if (key == "experience_enabled")
        c.experience_enabled = parse_bool(val);
    else if (key == "experience_store_path")
        c.experience_store_path = val;
    else if (key == "experience_max_memories")
        parse_int(key, val, warnings, c.experience_max_memories);
    else if (key == "experience_max_skills")
        parse_int(key, val, warnings, c.experience_max_skills);
    else if (key == "experience_decay_rate")
        parse_double(key, val, warnings, c.experience_decay_rate);
    else if (key == "experience_promote_threshold")
        parse_int(key, val, warnings, c.experience_promote_threshold);
}

bool valid_index_source(const std::string& value) {
    return value == "skills.sh" || value == "none" || value.rfind("http://", 0) == 0 ||
           value.rfind("https://", 0) == 0;
}

void apply_acquisition_keys(Config& c, const std::string& key, const std::string& val,
                            std::vector<std::string>& warnings) {
    if (key == "skills_index") {
        if (valid_index_source(val))
            c.skills_index = val;
        else
            warnings.emplace_back("skills_index: expected skills.sh, none or an http(s) URL");
    } else if (key == "skills_install_scope") {
        if (val == "project" || val == "global")
            c.skills_install_scope = val;
        else
            warnings.emplace_back("skills_install_scope: expected project or global");
    } else if (key == "skills_review_required") {
        c.skills_review_required = parse_bool(val);
    } else if (key == "skills_audit_required") {
        c.skills_audit_required = parse_bool(val);
    } else if (key == "skills_index_token_env") {
        c.skills_index_token_env = val;
    } else if (key == "skills_github_token_env") {
        c.skills_github_token_env = val;
    }
}

void apply_skills_keys(Config& c, const std::string& key, const std::string& val,
                       std::vector<std::string>& warnings) {
    if (key == "skills_interop")
        c.skills_interop = parse_bool(val);
    else if (key == "skills_max_discovery")
        parse_int(key, val, warnings, c.skills_max_discovery);
    else if (key == "skills_body_budget_tokens")
        parse_int(key, val, warnings, c.skills_body_budget_tokens);
    else
        apply_acquisition_keys(c, key, val, warnings);
}

void apply_logging_keys(Config& c, const std::string& key, const std::string& val) {
    if (key == "log_path")
        c.log_path = val;
    else if (key == "debug_log")
        c.debug_log = val;
    else if (key == "debug")
        c.debug = parse_bool(val);
}
} // namespace

void Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return;
    warnings.clear();
    std::string line;
    while (std::getline(in, line)) {
        Setting s = split_setting(line);
        if (s.key.empty())
            continue;
        apply_connection_keys(*this, s.key, s.value);
        apply_prompt_keys(*this, s.key, s.value);
        apply_sampling_keys(*this, s.key, s.value, warnings);
        apply_agent_keys(*this, s.key, s.value, warnings);
        apply_detection_keys(*this, s.key, s.value);
        apply_compression_keys(*this, s.key, s.value, warnings);
        apply_experience_keys(*this, s.key, s.value, warnings);
        apply_skills_keys(*this, s.key, s.value, warnings);
        apply_logging_keys(*this, s.key, s.value);
    }
}

void Config::load_workspace_defaults(const std::string& path) {
    // The provider's endpoint is not a project default: it is where the
    // selection says requests go. Snapshot it across the load.
    const bool from_provider = api_base_from_provider;
    const std::string endpoint = api_base;
    // Diagnostic OUTPUT paths are the operator's, not a repository's: a cloned
    // workspace's amber.conf must not be able to point the transcript or the
    // HTTP dump at a file amber would then append to or truncate (debug_log
    // truncates on its first write). $XDG_*/env, the global config and /set own
    // them; only this load is restricted, so those still work.
    const std::string operator_log_path = log_path;
    const std::string operator_debug_log = debug_log;
    load(path);
    log_path = operator_log_path;
    debug_log = operator_debug_log;
    if (from_provider)
        api_base = endpoint;
}

namespace {

// An environment-provided config root is only honoured when it is a sane
// absolute path with no parent-directory component. The value comes from the
// environment, so a malformed or hostile one must not redirect where amber
// reads and writes.
bool is_sane_config_root(const std::filesystem::path& p) {
    if (!p.is_absolute())
        return false;
    return std::all_of(p.begin(), p.end(),
                       [](const std::filesystem::path& part) { return part != ".."; });
}

// $HOME when it is usable, else the passwd entry. See user_home_dir().
std::string resolve_home() {
    const char* home = std::getenv("HOME");
    if (home && *home && is_sane_config_root(home))
        return home;
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir && *pw->pw_dir)
        return pw->pw_dir;
    return {};
}

} // namespace

std::string user_home_dir() {
    return resolve_home();
}

std::string global_config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg && is_sane_config_root(xdg))
        return (std::filesystem::path(xdg) / "amber").string();
    const std::string home = resolve_home();
    if (!home.empty())
        return (std::filesystem::path(home) / ".config" / "amber").string();
    // No HOME and no passwd entry: nothing can be chosen correctly, so keep the
    // historical relative name rather than inventing a new surprise.
    return ".amber";
}

std::string cache_dir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg && is_sane_config_root(xdg))
        return (std::filesystem::path(xdg) / "amber").string();
    const std::string home = resolve_home();
    if (!home.empty())
        return (std::filesystem::path(home) / ".cache" / "amber").string();
    return ".amber-cache";
}

std::string global_config_path() {
    return global_config_dir() + "/config";
}

// First-run bootstrap: write a commented default global config. The CLI and
// TUI call this at startup; the provider-domain defaults in Config already
// make amber usable with zero configuration, so the file is a jumping-off
// point, not a requirement. An empty `model=` keeps server auto-detection
// enabled.
bool ensure_global_config() {
    const std::string path = global_config_path();
    std::ifstream probe(path);
    if (probe)
        return false;
    std::ostringstream out;
    out << "# amber global settings (LLM provider)\n"
        << "# Uncomment and edit to connect to your LLM provider.\n"
        << "# provider=openai\n"
        << "# api_base=http://localhost:8081/v1\n"
        << "# api_key=\n"
        << "# Empty model = auto-detect from the server.\n"
        << "model=\n";
    return write_private_file(path, out.str());
}

bool Config::save_global(const std::string& path) const {
    std::ostringstream f;
    f << "# amber global settings (LLM provider)\n";
    f << "provider=" << provider_name << "\n";
    f << "api_base=" << api_base << "\n";
    f << "api_key=" << api_key << "\n";
    if (!kilo_balance_token.empty())
        f << "kilo_balance_token=" << kilo_balance_token << "\n";
    f << "model=" << model << "\n";
    // Only a user pin is written: an automatic resolution must not round-trip
    // into the file, because load() treats any value > 0 as explicit and the
    // next start would inherit a stale window as a pin.
    if (context_explicit)
        f << "context_size=" << context_size << "\n";
    f << "wallet=" << (wallet_enabled ? 1 : 0) << "\n";
    return write_private_file(path, f.str());
}

namespace {

// The compression keys are written only when the user set them explicitly, so an
// unset value never round-trips into the file and silently overrides a future
// default. Five conditional writes inside save_settings were the bulk of its
// branch count; the rule itself is one sentence, so it lives in one place.
void write_compression_settings(std::ostream& f, const Config& c) {
    if (c.compression_threshold_explicit)
        f << "compression_threshold=" << c.compression_threshold << "\n";
    if (c.compression_min_turns_explicit)
        f << "compression_min_turns=" << c.compression_min_turns << "\n";
    if (c.compression_cooldown_turns_explicit)
        f << "compression_cooldown_turns=" << c.compression_cooldown_turns << "\n";
    if (c.compression_target_pct_explicit)
        f << "compression_target_pct=" << c.compression_target_pct << "\n";
    if (c.compression_keep_last_prompts_explicit)
        f << "compression_keep_last_prompts=" << c.compression_keep_last_prompts << "\n";
}

} // namespace

bool Config::save_settings(const std::string& path) const {
    std::ostringstream f;
    f << "# amber project settings (local)\n";
    f << "max_tool_iterations=" << max_tool_iterations << "\n";
    f << "temperature=" << temperature << "\n";
    f << "max_tokens=" << max_tokens << "\n";
    f << "stream=" << (stream ? 1 : 0) << "\n";
    f << "thinking=" << thinking << "\n";
    f << "thinking_budget=" << thinking_budget << "\n";
    f << "reasoning_effort=" << reasoning_effort << "\n";
    write_compression_settings(f, *this);
    f << "show_reasoning=" << (show_reasoning ? 1 : 0) << "\n";
    f << "system_prompt=" << system_prompt_path << "\n";
    f << "tools_prompt=" << tools_prompt_path << "\n";
    f << "git_prompt=" << git_prompt_path << "\n";
    f << "log_path=" << log_path << "\n";
    f << "debug_log=" << debug_log << "\n";
    f << "debug=" << (debug ? 1 : 0) << "\n";
    f << "policy_approval=" << (policy_approval ? 1 : 0) << "\n";
    f << "detection_loop=" << (detection_loop ? 1 : 0) << "\n";
    f << "detection_duplicate=" << (detection_duplicate ? 1 : 0) << "\n";
    f << "skills_interop=" << (skills_interop ? 1 : 0) << "\n";
    f << "worktree=" << (worktree_auto ? 1 : 0) << "\n";
    f << "work_auto_attach=" << work_auto_attach << "\n";
    f << "skills_max_discovery=" << skills_max_discovery << "\n";
    f << "skills_body_budget_tokens=" << skills_body_budget_tokens << "\n";
    f << "skills_index=" << skills_index << "\n";
    f << "skills_install_scope=" << skills_install_scope << "\n";
    f << "skills_review_required=" << (skills_review_required ? 1 : 0) << "\n";
    f << "skills_audit_required=" << (skills_audit_required ? 1 : 0) << "\n";
    f << "max_tool_result_bytes=" << max_tool_result_bytes << "\n";
    return write_private_file(path, f.str());
}

namespace {

// An env var that must be absent, "1" or "true".
bool env_flag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v)
        return fallback;
    const std::string s(v);
    return s == "1" || s == "true";
}

// An env var parsed as an int; `fallback` when absent.
int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v ? std::atoi(v) : fallback;
}

// Assign a string field from an env var, leaving it alone when unset.
void env_string(const char* name, std::string& out) {
    const char* v = std::getenv(name);
    if (v)
        out = v;
}

} // namespace

void Config::apply_environment() {
    env_string("AMBER_API_BASE", api_base);
    env_string("AMBER_API_KEY", api_key);
    const std::string prev_model = model;
    env_string("AMBER_MODEL", model);
    if (model != prev_model)
        model_explicit = true;
    env_string("AMBER_GIT_PROMPT", git_prompt_path);
    env_string("AMBER_SYSTEM_PROMPT", system_prompt_path);
    env_string("AMBER_TOOLS_PROMPT", tools_prompt_path);
    env_string("AMBER_THINKING", thinking);
    env_string("AMBER_LOG", log_path);
    env_string("AMBER_DEBUG", debug_log);
    debug = env_flag("AMBER_DEBUG_ON", debug);
    env_string("AMBER_REASONING", reasoning_effort);

    stream = env_flag("AMBER_STREAM", stream);
    subagent_parallel = env_flag("AMBER_SUBAGENT_PARALLEL", subagent_parallel);
    subagent_max = env_int("AMBER_SUBAGENT_MAX", subagent_max);
    max_tool_result_bytes = env_int("AMBER_MAX_TOOL_RESULT_BYTES", max_tool_result_bytes);
    thinking_budget = env_int("AMBER_THINKING_BUDGET", thinking_budget);
    show_reasoning = env_flag("AMBER_SHOW_REASONING", show_reasoning);

    // AMBER_CONTEXT also marks the window explicit, so "auto" is not used.
    if (const char* cs = std::getenv("AMBER_CONTEXT")) {
        context_size = std::atoi(cs);
        context_explicit = true;
    }
}

std::string Config::debug_dump_path() const {
    std::string path = !debug_log.empty() ? debug_log
                                          : (debug ? Workspace::state_dir() + "/logs/debug-{ts}.log"
                                                   : std::string());
    if (path.empty())
        return path;
    const std::string token = "{ts}";
    const std::size_t at = path.find(token);
    if (at == std::string::npos)
        return path;
    // The transport opens the dump lazily and has no session id; one file per
    // PROCESS is the honest equivalent, and it is what the transcript path does
    // too. Without this the name stayed literally "debug-{ts}.log" and every
    // session of the project appended to one unbounded file.
    static const long long started = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    std::string out = path;
    out.replace(at, token.size(), std::to_string(started));
    return out;
}

std::vector<std::string> Config::validate() const {
    std::vector<std::string> errs;

    if (api_base.empty()) {
        errs.emplace_back("api_base is empty");
    } else if (api_base.rfind("http://", 0) != 0 && api_base.rfind("https://", 0) != 0) {
        errs.push_back("api_base must start with http:// or https:// (got: " + api_base + ")");
    } else if (api_base.back() == '/') {
        errs.push_back("api_base must not end with a trailing '/' (got: " + api_base + ")");
    }

    if (model.empty())
        errs.emplace_back("model is empty");

    if (max_tool_iterations < 1)
        errs.push_back(
            "max_tool_iterations must be >= 1 (got: " + std::to_string(max_tool_iterations) + ")");

    if (temperature < 0.0 || temperature > 2.0)
        errs.push_back("temperature must be in [0.0, 2.0] (got: " + std::to_string(temperature) +
                       ")");

    if (max_tokens == 0)
        errs.emplace_back("max_tokens must be > 0");

    if (thinking != "on" && thinking != "off" && thinking != "auto")
        errs.push_back("thinking must be one of on|off|auto (got: " + thinking + ")");

    return errs;
}

} // namespace agent

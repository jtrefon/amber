
#include "agent/config.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

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

void apply_skills_keys(Config& c, const std::string& key, const std::string& val,
                       std::vector<std::string>& warnings) {
    if (key == "skills_interop")
        c.skills_interop = parse_bool(val);
    else if (key == "skills_max_discovery")
        parse_int(key, val, warnings, c.skills_max_discovery);
    else if (key == "skills_body_budget_tokens")
        parse_int(key, val, warnings, c.skills_body_budget_tokens);
}

void apply_logging_keys(Config& c, const std::string& key, const std::string& val) {
    if (key == "log_path")
        c.log_path = val;
    else if (key == "debug_log")
        c.debug_log = val;
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

} // namespace

std::string global_config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg && is_sane_config_root(xdg))
        return (std::filesystem::path(xdg) / "amber").string();
    const char* home = std::getenv("HOME");
    if (home && *home && is_sane_config_root(home))
        return (std::filesystem::path(home) / ".config" / "amber").string();
    return ".amber";
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
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    f << "# amber global settings (LLM provider)\n"
      << "# Uncomment and edit to connect to your LLM provider.\n"
      << "# provider=openai\n"
      << "# api_base=http://localhost:8081/v1\n"
      << "# api_key=\n"
      << "# Empty model = auto-detect from the server.\n"
      << "model=\n";
    return static_cast<bool>(f);
}

bool Config::save_global(const std::string& path) const {
    std::error_code ec;
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    f << "# amber global settings (LLM provider)\n";
    f << "provider=" << provider_name << "\n";
    f << "api_base=" << api_base << "\n";
    f << "api_key=" << api_key << "\n";
    if (!kilo_balance_token.empty())
        f << "kilo_balance_token=" << kilo_balance_token << "\n";
    f << "model=" << model << "\n";
    f << "context_size=" << context_size << "\n";
    f << "wallet=" << (wallet_enabled ? 1 : 0) << "\n";
    return static_cast<bool>(f);
}

bool Config::save_settings(const std::string& path) const {
    // Ensure the parent directory exists (e.g. .amber/ for .amber/settings)
    std::error_code ec;
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    f << "# amber project settings (local)\n";
    f << "max_tool_iterations=" << max_tool_iterations << "\n";
    f << "temperature=" << temperature << "\n";
    f << "max_tokens=" << max_tokens << "\n";
    f << "stream=" << (stream ? 1 : 0) << "\n";
    f << "thinking=" << thinking << "\n";
    f << "thinking_budget=" << thinking_budget << "\n";
    f << "reasoning_effort=" << reasoning_effort << "\n";
    if (compression_threshold_explicit)
        f << "compression_threshold=" << compression_threshold << "\n";
    if (compression_min_turns_explicit)
        f << "compression_min_turns=" << compression_min_turns << "\n";
    if (compression_cooldown_turns_explicit)
        f << "compression_cooldown_turns=" << compression_cooldown_turns << "\n";
    if (compression_target_pct_explicit)
        f << "compression_target_pct=" << compression_target_pct << "\n";
    if (compression_keep_last_prompts_explicit)
        f << "compression_keep_last_prompts=" << compression_keep_last_prompts << "\n";
    f << "show_reasoning=" << (show_reasoning ? 1 : 0) << "\n";
    f << "system_prompt=" << system_prompt_path << "\n";
    f << "tools_prompt=" << tools_prompt_path << "\n";
    f << "git_prompt=" << git_prompt_path << "\n";
    f << "log_path=" << log_path << "\n";
    f << "debug_log=" << debug_log << "\n";
    f << "policy_approval=" << (policy_approval ? 1 : 0) << "\n";
    f << "detection_loop=" << (detection_loop ? 1 : 0) << "\n";
    f << "detection_duplicate=" << (detection_duplicate ? 1 : 0) << "\n";
    f << "skills_interop=" << (skills_interop ? 1 : 0) << "\n";
    f << "skills_max_discovery=" << skills_max_discovery << "\n";
    f << "skills_body_budget_tokens=" << skills_body_budget_tokens << "\n";
    return static_cast<bool>(f);
}

void Config::apply_environment() {
    auto get = [](const char* n, std::string& out) {
        const char* v = std::getenv(n);
        if (v)
            out = v;
    };
    get("AMBER_API_BASE", api_base);
    get("AMBER_API_KEY", api_key);
    {
        std::string prev = model;
        get("AMBER_MODEL", model);
        if (model != prev)
            model_explicit = true;
    }
    get("AMBER_GIT_PROMPT", git_prompt_path);
    get("AMBER_SYSTEM_PROMPT", system_prompt_path);
    get("AMBER_TOOLS_PROMPT", tools_prompt_path);
    const char* s = std::getenv("AMBER_STREAM");
    if (s)
        stream = (std::string(s) == "1" || std::string(s) == "true");
    const char* spa = std::getenv("AMBER_SUBAGENT_PARALLEL");
    if (spa)
        subagent_parallel = (std::string(spa) == "1" || std::string(spa) == "true");
    const char* sma = std::getenv("AMBER_SUBAGENT_MAX");
    if (sma)
        subagent_max = std::atoi(sma);
    get("AMBER_THINKING", thinking);
    const char* tb = std::getenv("AMBER_THINKING_BUDGET");
    if (tb)
        thinking_budget = std::atoi(tb);
    const char* cs = std::getenv("AMBER_CONTEXT");
    if (cs) {
        context_size = std::atoi(cs);
        context_explicit = true;
    }
    get("AMBER_LOG", log_path);
    get("AMBER_DEBUG", debug_log);
    get("AMBER_REASONING", reasoning_effort);
    const char* sr = std::getenv("AMBER_SHOW_REASONING");
    if (sr)
        show_reasoning = (std::string(sr) == "1" || std::string(sr) == "true");
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

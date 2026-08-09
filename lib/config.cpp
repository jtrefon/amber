
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
bool parse_num(const std::string& key, const std::string& val,
               std::vector<std::string>& warnings, long& out) {
    try {
        out = std::stol(val);
        return true;
    } catch (const std::exception&) {
        warnings.push_back(key + "=\"" + val + "\" is not a number; using default");
        return false;
    }
}
} // namespace

void Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    warnings.clear();
    auto parse_int = [this](const std::string& k, const std::string& v,
                            int& out) {
        long n = 0;
        if (parse_num(k, v, warnings, n)) out = static_cast<int>(n);
    };
    auto parse_uint = [this](const std::string& k, const std::string& v,
                             size_t& out) {
        long n = 0;
        if (parse_num(k, v, warnings, n) && n >= 0)
            out = static_cast<size_t>(n);
    };
    auto parse_double = [this](const std::string& k, const std::string& v,
                               double& out) {
        try {
            out = std::stod(v);
        } catch (const std::exception&) {
            warnings.push_back(k + "=\"" + v + "\" is not a number; using default");
        }
    };
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        if (key == "api_base") api_base = val;
        else if (key == "api_key") api_key = val;
        else if (key == "model") {
            // An empty model in the config means "auto-detect from the server";
            // do not treat it as an explicit choice (that would disable probing).
            model = val;
            model_explicit = !val.empty();
        }
        else if (key == "system_prompt") system_prompt_path = val;
        else if (key == "tools_prompt") tools_prompt_path = val;
        else if (key == "git_prompt") git_prompt_path = val;
        else if (key == "max_tool_iterations") parse_int(key, val, max_tool_iterations);
        else if (key == "temperature") parse_double(key, val, temperature);
        else if (key == "max_tokens") parse_uint(key, val, max_tokens);
        else if (key == "task_tool")
            task_tool = (val == "1" || val == "true" || val == "yes");
        else if (key == "subagent_parallel")
            subagent_parallel = (val == "1" || val == "true" || val == "yes");
        else if (key == "subagent_max") parse_int(key, val, subagent_max);
        else if (key == "plan_tool")
            plan_tool = (val == "1" || val == "true" || val == "yes");
        else if (key == "stream") stream = (val == "1" || val == "true" || val == "yes");
        else if (key == "thinking") thinking = val;
        else if (key == "thinking_budget") parse_int(key, val, thinking_budget);
        else if (key == "context_size") {
            // 0 (or negative) means "auto-detect"; only a positive value counts
            // as an explicit override that suppresses server probing.
            parse_int(key, val, context_size);
            context_explicit = context_size > 0;
        }
        else if (key == "default_context_size") {
            // Provider-level default; does NOT set context_explicit so
            // server auto-detect and user-override can still win.
            if (!val.empty()) parse_int(key, val, default_context_size);
        }
        else if (key == "log_path") log_path = val;
        else if (key == "debug_log") debug_log = val;
        else if (key == "reasoning_effort") reasoning_effort = val;
        else if (key == "show_reasoning")
            show_reasoning = (val == "1" || val == "true" || val == "yes");
        else if (key == "compression_threshold") {
            parse_double(key, val, compression_threshold);
            compression_threshold_explicit = true;
        } else if (key == "compression_min_turns") {
            parse_int(key, val, compression_min_turns);
            compression_min_turns_explicit = true;
        } else if (key == "compression_cooldown_turns") {
            parse_int(key, val, compression_cooldown_turns);
            compression_cooldown_turns_explicit = true;
        }
        else if (key == "experience_enabled")
            experience_enabled = (val == "1" || val == "true" || val == "yes");
        else if (key == "experience_store_path")
            experience_store_path = val;
        else if (key == "experience_max_memories")
            parse_int(key, val, experience_max_memories);
        else if (key == "experience_max_skills")
            parse_int(key, val, experience_max_skills);
        else if (key == "experience_decay_rate")
            parse_double(key, val, experience_decay_rate);
        else if (key == "experience_promote_threshold")
            parse_int(key, val, experience_promote_threshold);
        else if (key == "skills_interop")
            skills_interop = (val == "1" || val == "true" || val == "yes");
        else if (key == "skills_max_discovery")
            parse_int(key, val, skills_max_discovery);
        else if (key == "skills_body_budget_tokens")
            parse_int(key, val, skills_body_budget_tokens);
        else if (key == "provider")
            provider_name = val;
        else if (key == "policy_approval")
            policy_approval = (val == "1" || val == "true" || val == "yes");
        else if (key == "detection_loop")
            detection_loop = (val == "1" || val == "true" || val == "yes");
        else if (key == "detection_duplicate")
            detection_duplicate = (val == "1" || val == "true" || val == "yes");
    }
}

std::string global_config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/amber";
    const char* home = std::getenv("HOME");
    if (!home) return ".amber";
    return std::string(home) + "/.config/amber";
}

std::string global_config_path() {
    return global_config_dir() + "/config";
}

bool Config::save_global(const std::string& path) const {
    std::error_code ec;
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# amber global settings (LLM provider)\n";
    f << "provider=" << provider_name << "\n";
    f << "api_base=" << api_base << "\n";
    f << "api_key=" << api_key << "\n";
    f << "model=" << model << "\n";
    f << "context_size=" << context_size << "\n";
    return static_cast<bool>(f);
}

bool Config::save_settings(const std::string& path) const {
    // Ensure the parent directory exists (e.g. .amber/ for .amber/settings)
    std::error_code ec;
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
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
        if (v) out = v;
    };
    get("AMBER_API_BASE", api_base);
    get("AMBER_API_KEY", api_key);
    { std::string prev = model; get("AMBER_MODEL", model);
      if (model != prev) model_explicit = true; }
    get("AMBER_GIT_PROMPT", git_prompt_path);
    get("AMBER_SYSTEM_PROMPT", system_prompt_path);
    get("AMBER_TOOLS_PROMPT", tools_prompt_path);
    const char* s = std::getenv("AMBER_STREAM");
    if (s) stream = (std::string(s) == "1" || std::string(s) == "true");
    const char* pt = std::getenv("AMBER_PLAN_TOOL");
    if (pt) plan_tool = (std::string(pt) == "1" || std::string(pt) == "true");
    const char* tt = std::getenv("AMBER_TASK_TOOL");
    if (tt) task_tool = (std::string(tt) == "1" || std::string(tt) == "true");
    const char* spa = std::getenv("AMBER_SUBAGENT_PARALLEL");
    if (spa) subagent_parallel = (std::string(spa) == "1" || std::string(spa) == "true");
    const char* sma = std::getenv("AMBER_SUBAGENT_MAX");
    if (sma) subagent_max = std::atoi(sma);
    get("AMBER_THINKING", thinking);
    const char* tb = std::getenv("AMBER_THINKING_BUDGET");
    if (tb) thinking_budget = std::atoi(tb);
    const char* cs = std::getenv("AMBER_CONTEXT");
    if (cs) { context_size = std::atoi(cs); context_explicit = true; }
    get("AMBER_LOG", log_path);
    get("AMBER_DEBUG", debug_log);
    get("AMBER_REASONING", reasoning_effort);
    const char* sr = std::getenv("AMBER_SHOW_REASONING");
    if (sr) show_reasoning = (std::string(sr) == "1" || std::string(sr) == "true");
}

std::vector<std::string> Config::validate() const {
    std::vector<std::string> errs;

    if (api_base.empty()) {
        errs.emplace_back("api_base is empty");
    } else if (api_base.rfind("http://", 0) != 0 &&
               api_base.rfind("https://", 0) != 0) {
        errs.push_back("api_base must start with http:// or https:// (got: " +
                       api_base + ")");
    } else if (api_base.back() == '/') {
        errs.push_back("api_base must not end with a trailing '/' (got: " +
                       api_base + ")");
    }

    if (model.empty())
        errs.emplace_back("model is empty");

    if (max_tool_iterations < 1)
        errs.push_back("max_tool_iterations must be >= 1 (got: " +
                       std::to_string(max_tool_iterations) + ")");

    if (temperature < 0.0 || temperature > 2.0)
        errs.push_back("temperature must be in [0.0, 2.0] (got: " +
                       std::to_string(temperature) + ")");

    if (max_tokens == 0)
        errs.emplace_back("max_tokens must be > 0");

    if (thinking != "on" && thinking != "off" && thinking != "auto")
        errs.push_back("thinking must be one of on|off|auto (got: " +
                       thinking + ")");

    return errs;
}

} // namespace agent

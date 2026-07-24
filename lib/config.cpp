// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/config.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agent {

void Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
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
        else if (key == "max_tool_iterations") max_tool_iterations = std::stoi(val);
        else if (key == "temperature") temperature = std::stod(val);
        else if (key == "max_tokens") max_tokens = std::stoul(val);
        else if (key == "stream") stream = (val == "1" || val == "true" || val == "yes");
        else if (key == "thinking") thinking = val;
        else if (key == "thinking_budget") thinking_budget = std::stoi(val);
        else if (key == "context_size") {
            // 0 (or negative) means "auto-detect"; only a positive value counts
            // as an explicit override that suppresses server probing.
            context_size = std::stoi(val);
            context_explicit = context_size > 0;
        }
        else if (key == "log_path") log_path = val;
        else if (key == "debug_log") debug_log = val;
        else if (key == "reasoning_effort") reasoning_effort = val;
        else if (key == "show_reasoning")
            show_reasoning = (val == "1" || val == "true" || val == "yes");
        else if (key == "compression_threshold")
            compression_threshold = std::stod(val);
        else if (key == "compression_min_turns")
            compression_min_turns = std::stoi(val);
        else if (key == "compression_cooldown_turns")
            compression_cooldown_turns = std::stoi(val);
        else if (key == "experience_enabled")
            experience_enabled = (val == "1" || val == "true" || val == "yes");
        else if (key == "experience_max_memories")
            experience_max_memories = std::stoi(val);
        else if (key == "experience_max_skills")
            experience_max_skills = std::stoi(val);
        else if (key == "provider")
            provider_name = val;
        else if (key == "detection_loop")
            detection_loop = (val == "1" || val == "true" || val == "yes");
        else if (key == "detection_duplicate")
            detection_duplicate = (val == "1" || val == "true" || val == "yes");
    }
}

bool Config::save(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# amber settings\n";
    f << "api_base=" << api_base << "\n";
    f << "api_key=" << api_key << "\n";
    // Persist intent, not the auto-detected value: only write a concrete model /
    // context_size when the user set one explicitly. Otherwise write the
    // sentinels (empty / 0) so the next load() auto-detects again.
    f << "model=" << (model_explicit ? model : "") << "\n";
    f << "context_size=" << (context_explicit ? context_size : 0) << "\n";
    f << "system_prompt=" << system_prompt_path << "\n";
    f << "tools_prompt=" << tools_prompt_path << "\n";
    return static_cast<bool>(f);
}

static std::string global_config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/amber";
    const char* home = std::getenv("HOME");
    if (!home) return ".amber";
    return std::string(home) + "/.config/amber";
}

std::string global_config_path() {
    return global_config_dir() + "/config";
}

void Config::apply_provider(const std::string& name) {
    auto* p = provider::find(name);
    if (!p || p->name == "custom") return;
    provider_name = p->name;
    api_base = p->api_base;
    if (model.empty() || model == "gpt-4o-mini")
        model = p->default_model;
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
    f << "show_reasoning=" << (show_reasoning ? 1 : 0) << "\n";
    f << "system_prompt=" << system_prompt_path << "\n";
    f << "tools_prompt=" << tools_prompt_path << "\n";
    f << "log_path=" << log_path << "\n";
    f << "debug_log=" << debug_log << "\n";
    f << "detection_loop=" << (detection_loop ? 1 : 0) << "\n";
    f << "detection_duplicate=" << (detection_duplicate ? 1 : 0) << "\n";
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
    get("AMBER_SYSTEM_PROMPT", system_prompt_path);
    get("AMBER_TOOLS_PROMPT", tools_prompt_path);
    const char* s = std::getenv("AMBER_STREAM");
    if (s) stream = (std::string(s) == "1" || std::string(s) == "true");
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

    // Managed providers (OpenRouter, Kilo Code) require an API key.
    auto* prov = provider::find(provider_name);
    if (prov && prov->requires_key && api_key.empty())
        errs.emplace_back("api_key is required for " + provider_name +
                          " (set via AMBER_API_KEY env or save_global)");

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

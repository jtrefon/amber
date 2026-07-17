// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/config.h"
#include <cstdlib>
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
        else if (key == "model") { model = val; model_explicit = true; }
        else if (key == "system_prompt") system_prompt_path = val;
        else if (key == "tools_prompt") tools_prompt_path = val;
        else if (key == "max_tool_iterations") max_tool_iterations = std::stoi(val);
        else if (key == "temperature") temperature = std::stod(val);
        else if (key == "max_tokens") max_tokens = std::stoul(val);
        else if (key == "stream") stream = (val == "1" || val == "true" || val == "yes");
        else if (key == "thinking") thinking = val;
        else if (key == "thinking_budget") thinking_budget = std::stoi(val);
        else if (key == "context_size") {
            context_size = std::stoi(val);
            context_explicit = true;
        }
        else if (key == "log_path") log_path = val;
        else if (key == "debug_log") debug_log = val;
        else if (key == "reasoning_effort") reasoning_effort = val;
        else if (key == "show_reasoning")
            show_reasoning = (val == "1" || val == "true" || val == "yes");
    }
}

void Config::apply_environment() {
    auto get = [](const char* n, std::string& out) {
        const char* v = std::getenv(n);
        if (v) out = v;
    };
    get("CPP_AGENT_API_BASE", api_base);
    get("CPP_AGENT_API_KEY", api_key);
    { std::string prev = model; get("CPP_AGENT_MODEL", model);
      if (model != prev) model_explicit = true; }
    get("CPP_AGENT_SYSTEM_PROMPT", system_prompt_path);
    get("CPP_AGENT_TOOLS_PROMPT", tools_prompt_path);
    const char* s = std::getenv("CPP_AGENT_STREAM");
    if (s) stream = (std::string(s) == "1" || std::string(s) == "true");
    get("CPP_AGENT_THINKING", thinking);
    const char* tb = std::getenv("CPP_AGENT_THINKING_BUDGET");
    if (tb) thinking_budget = std::atoi(tb);
    const char* cs = std::getenv("CPP_AGENT_CONTEXT");
    if (cs) { context_size = std::atoi(cs); context_explicit = true; }
    get("CPP_AGENT_LOG", log_path);
    get("CPP_AGENT_DEBUG", debug_log);
    get("CPP_AGENT_REASONING", reasoning_effort);
    const char* sr = std::getenv("CPP_AGENT_SHOW_REASONING");
    if (sr) show_reasoning = (std::string(sr) == "1" || std::string(sr) == "true");
}

} // namespace agent

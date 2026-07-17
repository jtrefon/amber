// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include <agent.h>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] [prompt]\n\n"
              << "  --api-base URL     OpenAI-compatible base URL (env CPP_AGENT_API_BASE)\n"
              << "  --api-key KEY      API key (env CPP_AGENT_API_KEY)\n"
              << "  --model NAME       Model name (env CPP_AGENT_MODEL)\n"
              << "  --system FILE      System prompt markdown file\n"
              << "  --tools FILE       Tools advertising markdown file\n"
              << "  --config FILE      KEY=VALUE config file\n"
              << "  --prompt TEXT      User prompt (else read from stdin)\n"
              << "  -h, --help         Show this help\n";
}

} // namespace

int main(int argc, char** argv) {
    agent::Config cfg;
    std::string prompt;
    std::string config_file;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            if (i + 1 < argc) return argv[++i];
            return def;
        };
        if (a == "--api-base")        cfg.api_base = next("");
        else if (a == "--api-key")    cfg.api_key = next("");
        else if (a == "--model")      { cfg.model = next(""); cfg.model_explicit = true; }
        else if (a == "--system")     cfg.system_prompt_path = next("");
        else if (a == "--tools")      cfg.tools_prompt_path = next("");
        else if (a == "--config")     config_file = next("");
        else if (a == "--prompt")     prompt = next("");
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
        else if (prompt.empty())      prompt = a;
        else { prompt += " " + a; }
    }

    if (!config_file.empty()) cfg.load(config_file);
    cfg.apply_environment();

    // Auto-detect model / context window from the server, filling only values
    // the user did not set explicitly.
    {
        agent::ServerInfo info = agent::apply_server_autodetect(cfg);
        if (info.ok)
            std::cerr << "[server] model=" << cfg.model
                      << " n_ctx=" << cfg.context_size << "\n";
    }

    if (prompt.empty()) {
        std::getline(std::cin, prompt);
    }
    if (prompt.empty()) {
        std::cerr << "error: no prompt provided\n";
        return 1;
    }

    if (cfg.system_prompt_path.empty())
        cfg.system_prompt_path = "prompts/system.md";
    if (cfg.tools_prompt_path.empty())
        cfg.tools_prompt_path = "prompts/tools.md";

    agent::ToolRegistry registry;
    agent::register_default_tools(registry);

    agent::AgentHooks hooks;
    hooks.on_status = [](const std::string& s) {
        std::cerr << "[status] " << s << "\n";
    };
    hooks.on_tool_call = [](const std::string& n, const agent::json& args) {
        std::cerr << "[tool] " << n << " " << args.dump() << "\n";
    };
    hooks.on_tool_result = [](const std::string& n, const agent::ToolResult& r) {
        std::cerr << "[result:" << n << "] "
                  << (r.ok ? r.output : r.error) << "\n";
    };

    try {
        agent::Agent agent(cfg, registry, hooks);
        std::string reply = agent.run(prompt);
        std::cout << "\n" << reply << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

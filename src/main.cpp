// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include <agent.h>
#include <agent/compressor.h>
#include <agent/experience.h>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] [prompt]\n\n"
              << "  --api-base URL     OpenAI-compatible base URL (env AMBER_API_BASE)\n"
              << "  --api-key KEY      API key (env AMBER_API_KEY)\n"
              << "  --model NAME       Model name (env AMBER_MODEL)\n"
              << "  --system FILE      System prompt markdown file\n"
              << "  --tools FILE       Tools advertising markdown file\n"
              << "  --config FILE      KEY=VALUE config file\n"
              << "  --prompt TEXT      User prompt (else read from stdin)\n"
              << "  --yes              Auto-approve tools that need confirmation (e.g. bash)\n"
              << "  --version          Print version and exit\n"
              << "  -h, --help         Show this help\n";
}

} // namespace

int main(int argc, char** argv) {
    agent::Config cfg;
    std::string prompt;
    std::string config_file;
    bool auto_approve = false;

    // Load the project config by default so `amber` works without --config.
    // Explicit flags and --config override these; the file is only a base.
    {
        std::ifstream def("amber.conf");
        if (def) cfg.load("amber.conf");
    }

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
        else if (a == "--yes" || a == "--yolo") auto_approve = true;
        else if (a == "--version") {
            std::cout << "amber " << agent::kVersion << " (" << agent::kBuildDate
                      << ")\n";
            return 0;
        }
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
        else if (prompt.empty())      prompt = a;
        else { prompt += " " + a; }
    }

    if (!config_file.empty()) cfg.load(config_file);
    cfg.apply_environment();

    // Auto-detect model / context window from the server first, filling only
    // values the user did not set explicitly. Done before validation so a blank
    // (auto) model can be resolved from the server rather than failing.
    {
        agent::ServerInfo info = agent::apply_server_autodetect(cfg);
        if (info.ok)
            std::cerr << "[server] model=" << cfg.model
                      << " n_ctx=" << cfg.context_size << "\n";
    }

    if (auto errs = cfg.validate(); !errs.empty()) {
        std::cerr << "error: invalid configuration:\n";
        for (const auto& e : errs) std::cerr << "  - " << e << "\n";
        return 2;
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
    agent::JobService jobs;
    agent::register_default_tools(registry, jobs, cfg.cancel_token);

    agent::AgentHooks hooks;
    hooks.on_status = [](const std::string& s) {
        std::cerr << "[status] " << s << "\n";
    };
    // Live streaming: surface tokens as they arrive so a long generation shows
    // progress instead of appearing to hang.
    hooks.on_token = [](const std::string& t) { std::cout << t << std::flush; };
    hooks.on_reasoning = [](const std::string& t) {
        std::cerr << "[think] " << t;
    };
    hooks.on_tool_call = [](const std::string& n, const agent::json& args) {
        std::cout.flush();
        std::cerr << "[tool] " << n << " " << args.dump() << "\n";
    };
    hooks.on_tool_result = [](const std::string& n, const agent::ToolResult& r) {
        std::cerr << "[result:" << n << "] "
                  << (r.ok ? r.output : r.error) << "\n";
    };
    // Approval gate for side-effecting tools (bash). With --yes, grant for the
    // session. Otherwise prompt on a TTY; if stdin is not interactive, deny
    // (fail-safe: never run shell commands unattended).
    bool tty = isatty(STDIN_FILENO);
    hooks.on_approval = [auto_approve, tty](const std::string&, const agent::json&,
                                            const std::string& summary) -> agent::Approval {
        if (auto_approve) return agent::Approval::AllowSession;
        if (!tty) {
            std::cerr << "[denied] " << summary
                      << "  (approval required; re-run with --yes to allow)\n";
            return agent::Approval::Deny;
        }
        std::cerr << "\n[approval] the agent wants to " << summary << "\n"
                  << "  allow? [y]es once / [a]llow session / [N]o: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line) || line.empty())
            return agent::Approval::Deny;
        char c = static_cast<char>(std::tolower(line[0]));
        if (c == 'y') return agent::Approval::AllowOnce;
        if (c == 'a') return agent::Approval::AllowSession;
        return agent::Approval::Deny;
    };

    // Build compression + experience pipeline.
    // The compressor receives the LLM client at call time (per compress()),
    // not at construction — no client reference needed here.
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto exp_cfg = agent::load_experience_config(cfg);
    auto mem_store = agent::make_memory_store(exp_cfg);
    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem_store);

    try {
        agent::Agent agent(cfg, registry, hooks,
                           std::move(compressor), std::move(gate),
                           std::move(mem_store), std::move(retriever));
        std::string reply = agent.run(prompt);
        std::cout << "\n" << reply << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}


#include <agent.h>

#include "agent/workspace.h"
#include "agent/bootstrap.h"
#include "agent/data_path.h"

#include "tui.h"

#include <clocale>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "\n"
                 "  amber — interactive terminal UI for the amber agent.\n"
                 "\n"
                 "Options:\n"
                 "  -v, --version      Print version and exit\n"
                 "  -h, --help         Show this help\n"
                 "  --config <file>    Load an explicit config file\n"
                 "  --api-base <url>   LLM API base URL\n"
                 "  --api-key <key>    LLM API key\n"
                 "  --model <name>     LLM model\n"
                 "  --system <file>    System prompt path\n"
                 "  --tools <file>     Tools prompt path\n"
                 "  --no-stream        Disable streaming responses\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    // ncursesw (wide-char) only operates in UTF-8 mode once the process locale
    // is set; without this it stays in the "C" locale and drops/mangles every
    // multi-byte glyph (em dash, bullets, box-drawing, CJK), which read as
    // "missing letters" and broken output across the whole session. Try the
    // environment locale first, then fall back to explicit UTF-8 locales so a
    // malformed/missing LC_CTYPE (e.g. bare "UTF-8") does not silently break
    // wcwidth() and smear every non-ASCII glyph.
    if (!std::setlocale(LC_ALL, ""))
        std::setlocale(LC_ALL, "C.UTF-8");
    if (!std::setlocale(LC_ALL, ""))
        std::setlocale(LC_ALL, "en_US.UTF-8");

    // Version/help must work even when the data files the UI needs are
    // missing or misplaced, so handle them before any config or bootstrap
    // work. Otherwise `amber -v` on a broken install dies with "critical
    // data files missing" instead of answering.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v" || a == "--version") {
            std::printf("amber %s (%s)\n", agent::kVersion, agent::kBuildDate);
            return 0;
        }
        if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
    }

    agent::Config cfg;
    std::string config_file;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_file = argv[++i];
        else if (a == "--api-base" && i + 1 < argc) cfg.api_base = argv[++i];
        else if (a == "--api-key" && i + 1 < argc) cfg.api_key = argv[++i];
        else if (a == "--model" && i + 1 < argc) { cfg.model = argv[++i]; cfg.model_explicit = true; }
        else if (a == "--system" && i + 1 < argc) cfg.system_prompt_path = argv[++i];
        else if (a == "--tools" && i + 1 < argc) cfg.tools_prompt_path = argv[++i];
        else if (a == "--no-stream") cfg.stream = false;
    }
    if (!config_file.empty()) cfg.load(config_file);

    // Global settings: LLM provider config lives in ~/.config/amber/config.
    // This is loaded first so project-level and env overrides can layer on top.
    agent::Config tmp;
    {
        std::string global_path = agent::global_config_path();
        std::ifstream sf(global_path);
        if (sf) tmp.load(global_path);
        // The provider domain resolves the active provider and auto-loads
        // its last-used model; the user's explicit choice is remembered
        // per provider (default_model), so it survives restarts. The
        // user's explicit context window applies regardless of the
        // provider default.
        if (tmp.context_explicit && tmp.context_size > 0) {
            cfg.context_size = tmp.context_size;
            cfg.context_explicit = true;
        }
        auto providers = agent::make_default_provider_service(cfg);
        if (!tmp.provider_name.empty()) {
            auto sel = providers->select(tmp.provider_name);
            if (sel.ok()) agent::apply_selection(cfg, sel);
        }

        // Project-level amber.conf may still pin data paths; endpoint and
        // model come from the provider domain above.
        std::ifstream sf2("amber.conf");
        if (sf2) cfg.load("amber.conf");
    }

    // First run with no config: write a commented default so there is a file
    // to edit. Never touches an existing config.
    if (config_file.empty() && agent::ensure_global_config())
        std::fprintf(stderr, "info: wrote default config to %s\n",
                     agent::global_config_path().c_str());

    // Project-local overrides (non-LLM settings) live in .amber/settings so they
    // stay with the project while provider config remains global.
    {
        std::ifstream sf(agent::Workspace::local_dir() + "/settings");
        if (sf) cfg.load(agent::Workspace::local_dir() + "/settings");
    }
    cfg.apply_environment();

    agent::apply_server_autodetect(cfg);

    if (auto errs = cfg.validate(); !errs.empty()) {
        std::fprintf(stderr, "error: invalid configuration:\n");
        for (const auto& e : errs)
            std::fprintf(stderr, "  - %s\n", e.c_str());
        return 2;
    }

    if (cfg.system_prompt_path.empty())
        cfg.system_prompt_path = agent::resolve_data_path("prompts/system.md", argv[0]);
    else
        cfg.system_prompt_path = agent::resolve_data_path(cfg.system_prompt_path, argv[0]);
    if (cfg.tools_prompt_path.empty())
        cfg.tools_prompt_path = agent::resolve_data_path("prompts/tools.md", argv[0]);
    else
        cfg.tools_prompt_path = agent::resolve_data_path(cfg.tools_prompt_path, argv[0]);

    // Fail fast before the UI: prompts and the command tree are critical.
    if (auto missing = agent::missing_bootstrap_files(cfg, argv[0], true);
        !missing.empty()) {
        std::fprintf(stderr, "error: critical data files missing:\n");
        for (const auto& m : missing) std::fprintf(stderr, "  - %s\n", m.c_str());
        return 2;
    }

    agent::ToolRegistry registry;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(registry, jobs, todos, cfg.cancel_token,
                        cfg.plan_tool, subagents, cfg.task_tool);
    subagents.set_config(cfg);
    subagents.set_parallel(cfg.subagent_parallel);
    subagents.set_max(cfg.subagent_max);

    agent::PluginManager plugins;
    plugins.discover();
    agent::PluginRegistry plugin_reg;

    tui::Tui tui(cfg, registry, jobs, subagents, plugins, plugin_reg);
    tui.run();
    return 0;
}

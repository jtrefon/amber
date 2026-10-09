
#include <agent.h>

#include "agent/workspace.h"
#include "agent/model_probe.h"
#include "agent/plugin_runtime.h"
#include "agent/bootstrap.h"
#include "agent/data_path.h"

#include "tui.h"

#include <algorithm>
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

// ncursesw (wide-char) only operates in UTF-8 mode once the process locale is
// set; without this it stays in the "C" locale and drops/mangles every
// multi-byte glyph (em dash, bullets, box-drawing, CJK), which read as "missing
// letters" and broken output across the whole session. Try the environment
// locale first, then fall back to explicit UTF-8 locales so a malformed/missing
// LC_CTYPE (e.g. bare "UTF-8") does not silently break wcwidth() and smear every
// non-ASCII glyph.
void setup_locale() {
    if (!std::setlocale(LC_ALL, ""))
        std::setlocale(LC_ALL, "C.UTF-8");
    if (!std::setlocale(LC_ALL, ""))
        std::setlocale(LC_ALL, "en_US.UTF-8");
}

// Version/help must work even when the data files the UI needs are missing or
// misplaced, so they are handled before any config or bootstrap work. Otherwise
// `amber -v` on a broken install dies with "critical data files missing" instead
// of answering. Returns an exit code when a flag asked to stop.
std::optional<int> handle_version_help(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--version") {
            std::printf("amber %s (%s)\n", agent::kVersion, agent::kBuildDate);
            return 0;
        }
        if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }
    return std::nullopt;
}

struct TuiCliArgs {
    std::string config_file;
};

// Consume the next argument as this flag's value. Returns false — leaving the
// target alone — when the flag is the last argument, which is how a trailing
// value-less flag has always been ignored here.
bool take_value(int argc, char** argv, int& i, std::string& out) {
    if (i + 1 >= argc)
        return false;
    out = argv[++i];
    return true;
}

// Flags that consume the next argument. Unknown flags stay ignored, as before.
void parse_tui_args(int argc, char** argv, agent::Config& cfg, TuiCliArgs& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config")
            take_value(argc, argv, i, args.config_file);
        else if (a == "--api-base")
            take_value(argc, argv, i, cfg.api_base);
        else if (a == "--api-key")
            take_value(argc, argv, i, cfg.api_key);
        else if (a == "--model") {
            if (take_value(argc, argv, i, cfg.model))
                cfg.model_explicit = true;
        } else if (a == "--system")
            take_value(argc, argv, i, cfg.system_prompt_path);
        else if (a == "--tools")
            take_value(argc, argv, i, cfg.tools_prompt_path);
        else if (a == "--no-stream")
            cfg.stream = false;
    }
}

// Global settings: LLM provider config lives in ~/.config/amber/config. This is
// loaded first so project-level and env overrides can layer on top. The provider
// domain resolves the active provider and auto-loads its last-used model; the
// user's explicit choice is remembered per provider (default_model), so it
// survives restarts. The user's explicit context window applies regardless of
// the provider default.
void apply_global_config(agent::Config& cfg) {
    agent::Config tmp;
    const std::string global_path = agent::global_config_path();
    std::ifstream sf(global_path);
    if (sf)
        tmp.load(global_path);
    if (tmp.context_explicit && tmp.context_size > 0) {
        cfg.context_size = tmp.context_size;
        cfg.context_explicit = true;
    }
    auto providers = agent::make_default_provider_service(cfg);
    if (!tmp.provider_name.empty()) {
        auto sel = providers->select(tmp.provider_name);
        if (sel.ok())
            agent::apply_selection(cfg, sel);
    }
    // Project-level amber.conf may still pin data paths; endpoint and model come
    // from the provider domain above.
    std::ifstream sf2("amber.conf");
    if (sf2)
        cfg.load("amber.conf");
}

// Project-local overrides (non-LLM settings) live in the per-project state dir
// while provider config remains global.
void apply_local_settings(agent::Config& cfg) {
    const std::string path = agent::Workspace::settings_path();
    std::ifstream sf(path);
    if (sf)
        cfg.load(path);
    cfg.apply_environment();
}

// The TUI tolerates an unresolved model — the background catalog refresh or
// /set model can fix it in-app — so it is dropped from the validation errors
// rather than gating startup on a network round-trip.
bool validate_for_tui(const agent::Config& cfg) {
    auto errs = cfg.validate();
    errs.erase(std::remove(errs.begin(), errs.end(), "model is empty"), errs.end());
    if (errs.empty())
        return true;
    std::fprintf(stderr, "error: invalid configuration:\n");
    for (const auto& e : errs)
        std::fprintf(stderr, "  - %s\n", e.c_str());
    return false;
}

// Resolve the prompt files next to the binary, or as the user wrote them.
void resolve_prompt_paths(agent::Config& cfg, const char* argv0) {
    cfg.system_prompt_path = agent::resolve_data_path(
        cfg.system_prompt_path.empty() ? "prompts/system.md" : cfg.system_prompt_path, argv0);
    cfg.tools_prompt_path = agent::resolve_data_path(
        cfg.tools_prompt_path.empty() ? "prompts/tools.md" : cfg.tools_prompt_path, argv0);
}

// Everything the TUI needs to exist for its lifetime, in construction order: the
// runtime is built from the registry, config and workspace, and the tool plugins
// construct the built-in tools from the host services, so those are attached
// before activation. Activation itself happens in the Tui constructor, which is
// where the live config lands.
struct TuiHost {
    agent::ToolRegistry registry;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::PluginManager plugins;
    agent::Workspace workspace;
    agent::PluginRuntime plugin_runtime;
    agent::HostServices host_services;

    explicit TuiHost(agent::Config& cfg)
        : plugin_runtime(registry, cfg, workspace),
          host_services{&jobs, &todos, &subagents, &cfg.cancel_token} {
        subagents.set_config(cfg);
        subagents.set_parallel(cfg.subagent_parallel);
        subagents.set_max(cfg.subagent_max);
        plugins.discover();
        // The v2 runtime: bundled plugins register here, their state comes from
        // ~/.config/amber/plugins/<id>/plugin.conf, and activation installs what
        // each declares into the shared registries.
        plugin_runtime.add_bundled();
        plugin_runtime.add_external(plugins);
        plugin_runtime.attach_host_services(host_services);
    }
};

} // namespace

int main(int argc, char** argv) try {
    setup_locale();
    if (std::optional<int> rc = handle_version_help(argc, argv))
        return *rc;

    agent::Config cfg;
    TuiCliArgs args;
    parse_tui_args(argc, argv, cfg, args);
    if (!args.config_file.empty())
        cfg.load(args.config_file);
    apply_global_config(cfg);

    // First run with no config: write a commented default so there is a file to
    // edit. Never touches an existing config.
    if (args.config_file.empty() && agent::ensure_global_config())
        std::fprintf(stderr, "info: wrote default config to %s\n",
                     agent::global_config_path().c_str());
    apply_local_settings(cfg);

    // Startup is paint-first: the model catalog is read from the disk cache only,
    // so no network I/O ever blocks this path. A missing or stale catalog
    // revalidates on a background worker from Tui::detect_server().
    agent::apply_cached_server_autodetect(cfg);
    if (!validate_for_tui(cfg))
        return 2;
    resolve_prompt_paths(cfg, argv[0]);

    // Fail fast before the UI: prompts and the command tree are critical.
    if (auto missing = agent::missing_bootstrap_files(cfg, argv[0], true); !missing.empty()) {
        std::fprintf(stderr, "error: critical data files missing:\n");
        for (const auto& m : missing)
            std::fprintf(stderr, "  - %s\n", m.c_str());
        return 2;
    }

    TuiHost host(cfg);
    tui::Tui tui(cfg, host.registry, host.jobs, host.subagents, host.plugins, host.plugin_runtime);
    tui.run();
    return 0;
} catch (const std::exception& e) {
    // Tui's destructor has already run endwin() by the time this executes, so
    // the terminal is restored and a plain message is safe.
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
}

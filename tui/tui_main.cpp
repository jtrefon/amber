// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include <agent.h>

#include "agent/workspace.h"

#include "tui.h"

#include <clocale>
#include <cstdio>
#include <fstream>

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
    {
        std::ifstream sf("amber.conf");
        if (sf) cfg.load("amber.conf");
    }
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

    if (cfg.system_prompt_path.empty()) cfg.system_prompt_path = "prompts/system.md";
    if (cfg.tools_prompt_path.empty()) cfg.tools_prompt_path = "prompts/tools.md";

    agent::ToolRegistry registry;
    agent::JobService jobs;
    agent::register_default_tools(registry, jobs);

    tui::Tui tui(cfg, registry, jobs);
    tui.run();
    return 0;
}

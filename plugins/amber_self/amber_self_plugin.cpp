
#include "amber_self_plugin.h"

#include "agent/config_paths.h"
#include "agent/extensions.h"

namespace agent::plugins {

namespace {

// The body is rendered from config_paths() -- the same table /get config paths
// prints -- so a path is written in exactly one place in the codebase. The
// prose around it is the part that cannot be generated: what each file is FOR,
// and which command changes it.
std::string render_body() {
    std::string body;
    body += "# Amber's own configuration\n\n";
    body += "Amber keeps its files in four places, and the distinction matters when you\n";
    body += "change one: global config is shared by every project, project config lives\n";
    body += "with the checkout and may be committed, state is private to this user, and\n";
    body += "the cache is regenerable.\n\n";
    for (const ConfigPath& p : config_paths()) {
        body += "- " + p.what + ": `" + p.path + "`";
        if (!p.note.empty())
            body += " -- " + p.note;
        body += '\n';
    }
    body += '\n';
    body += "## Changing things\n\n";
    body += "- Providers (endpoint, key, model, protocol): `/provider <name>`, or\n";
    body += "  `/settings`. Each provider is one file under `providers/`; the\n";
    body += "  `flavor=` line is its wire protocol (openai, anthropic, gemini) and is\n";
    body += "  written by amber, not by hand.\n";
    body += "- The active model: `/set model` (per window), or `model=` in the global\n";
    body += "  config file.\n";
    body += "- Anything under `/set ...` (temperature, compression, display, policy):\n";
    body += "  the same command writes the file it belongs to; `/get ...` reads it back.\n";
    body += "- Skills: `/set skills create|delete|install`, then `/set skills show`.\n";
    body += "- Sessions: `/session list|load|delete`; the files are under `sessions/`.\n";
    body += "- MCP servers: `/mcp ...`; the files are under `mcp/`.\n";
    body += '\n';
    body += "A file may be edited directly (it is plain `key=value`), but the command\n";
    body += "that owns it is the safer route: it reloads what it wrote, and it is what\n";
    body += "`/get config paths` documents. `AMBER_WORKSPACE` moves the project root;\n";
    body += "`XDG_CONFIG_HOME`, `XDG_STATE_HOME` and `XDG_CACHE_HOME` move the rest.\n";
    return body;
}

} // namespace

std::string render_amber_config_skill() {
    return render_body();
}

std::vector<std::unique_ptr<Capability>> AmberSelfPlugin::capabilities() {
    SystemSkill skill;
    skill.name = "amber-config";
    skill.description =
        "Where amber keeps its own config, state and cache, and which command changes each";
    skill.body = [] { return render_amber_config_skill(); };
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<SkillCapability>(std::move(skill)));
    return caps;
}

} // namespace agent::plugins

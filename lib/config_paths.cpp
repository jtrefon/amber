
#include "agent/config_paths.h"

#include "agent/config.h"
#include "agent/workspace.h"

#include <utility>

namespace agent {

namespace {

ConfigPath row(std::string what, std::string path, std::string note) {
    return ConfigPath{std::move(what), std::move(path), std::move(note)};
}

} // namespace

std::vector<ConfigPath> config_paths() {
    const std::string config = global_config_dir();
    const std::string state = Workspace::state_dir();
    const std::string project = Workspace::local_dir();

    std::vector<ConfigPath> out;
    // Global: shared by every project, in the XDG config root.
    out.push_back(row("config", config + "/config",
                      "provider, model and keys shared by every project; /provider and /set "
                      "write it"));
    out.push_back(row("providers", config + "/providers",
                      "one <name>.conf per provider, keyed by provider name; /provider "
                      "<name> writes these"));
    out.push_back(
        row("global-skills", config + "/skills", "SKILL.md packages available in every project"));
    out.push_back(row("global-mcp", config + "/mcp", "MCP servers available in every project"));
    out.push_back(row("plugin-state", config + "/plugins",
                      "on/off state per plugin (/set plugin on|off <id>)"));
    out.push_back(row("skill-overrides", config + "/skills.json",
                      "per-skill enable/disable/block (/set skills)"));

    // Project: what a repository may share, in the project tree.
    out.push_back(row("project-config", project,
                      "shareable project config (skills, MCP, plugins); created only when one "
                      "is added"));
    out.push_back(row("project-skills", project + "/skills", "SKILL.md packages for this project"));
    out.push_back(row("project-mcp", project + "/mcp", "MCP servers for this project"));

    // State: private, per project, in the XDG state root.
    out.push_back(row("state", state, "private per-project state; never part of the repository"));
    out.push_back(row("sessions", state + "/sessions", "conversation history (/session list)"));
    out.push_back(row("settings", state + "/settings",
                      "project-local non-LLM settings (temperature, prompts, compression)"));
    out.push_back(row("logs", state + "/logs", "per-session transcripts"));
    out.push_back(
        row("experience", state + "/experience.json", "memories and learned skills (/get learn)"));
    out.push_back(row("policy", state + "/policy.json", "remembered approval rules (/set policy)"));

    // Cache: regenerable, in the XDG cache root.
    out.push_back(row("cache", cache_dir(), "regenerable model catalogues; safe to delete"));
    return out;
}

} // namespace agent

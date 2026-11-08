
#include "agent/config_paths.h"

#include "agent/config.h"
#include "agent/workspace.h"

#include <utility>

namespace agent {

namespace {

ConfigPath row(std::string what, std::string path, std::string note) {
    return ConfigPath{std::move(what), std::move(path), std::move(note)};
}

void add_global_paths(std::vector<ConfigPath>& out, const std::string& config) {
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
}

void add_project_paths(std::vector<ConfigPath>& out, const std::string& project) {
    out.push_back(row("project-config", project,
                      "shareable project config (skills, MCP, plugins); created only when one "
                      "is added"));
    out.push_back(row("project-skills", project + "/skills", "SKILL.md packages for this project"));
    out.push_back(row("project-mcp", project + "/mcp", "MCP servers for this project"));
}

void add_state_paths(std::vector<ConfigPath>& out, const std::string& state) {
    out.push_back(row("state", state, "private per-project state; never part of the repository"));
    out.push_back(row("sessions", state + "/sessions", "conversation history (/session list)"));
    out.push_back(row("settings", state + "/settings",
                      "project-local non-LLM settings (temperature, prompts, compression)"));
    out.push_back(row("logs", state + "/logs",
                      "per-session transcripts; bounded by /set logs retention|max"));
    out.push_back(row("errors", error_log_path(),
                      "app failures and recovered errors; written only with /set debug on"));
    out.push_back(
        row("experience", state + "/experience.json", "memories and learned skills (/get learn)"));
    out.push_back(row("policy", state + "/policy.json", "remembered approval rules (/set policy)"));
    out.push_back(row("tasks", task_records_dir(state),
                      "one private JSON record per background task; /tasks reads it and "
                      "TaskStore writes it"));
}

void add_cache_paths(std::vector<ConfigPath>& out) {
    out.push_back(row("cache", cache_dir(), "regenerable model catalogues; safe to delete"));
    out.push_back(row("task-artifacts", task_artifacts_dir(cache_dir()),
                      "full command output, worker transcripts and watch journals; pruned by "
                      "age and size"));
}

} // namespace

std::string error_log_path() {
    return Workspace::state_dir() + "/logs/errors.jsonl";
}

std::string task_records_dir(const std::string& state_dir) {
    return state_dir + "/tasks";
}

std::string task_artifacts_dir(const std::string& cache_root) {
    return cache_root + "/tasks";
}

std::vector<ConfigPath> config_paths() {
    std::vector<ConfigPath> out;
    add_global_paths(out, global_config_dir());
    add_project_paths(out, Workspace::local_dir());
    add_state_paths(out, Workspace::state_dir());
    add_cache_paths(out);
    return out;
}

} // namespace agent

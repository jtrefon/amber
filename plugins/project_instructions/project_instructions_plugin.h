
#ifndef AGENT_PLUGINS_PROJECT_INSTRUCTIONS_H
#define AGENT_PLUGINS_PROJECT_INSTRUCTIONS_H

#include "agent/plugin_core.h"

#include <string>
#include <vector>

namespace agent::plugins {

// The project's own standing instructions (AGENTS.md), as a session-fixed
// prompt block.
//
// AGENTS.md is the cross-tool convention every coding agent reads: a plain
// markdown file at the repository root describing how the project is built,
// tested and changed. Without it an agent re-derives conventions by guesswork —
// or never learns them — which is exactly the failure this plugin removes.
//
// The file is read once and cached, because the block rides in the system
// prompt on every turn: a per-render read would move the KV prefix whenever
// anything on disk changed. Disabling the plugin removes the block; a fresh
// session (or a disable/enable) reloads it.
class ProjectInstructionsPlugin : public IPlugin {
public:
    std::string id() const override { return "instructions"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Project instructions"; }
    std::string description() const override {
        return "AGENTS.md: the conventions, commands and out-of-bounds paths a project states for "
               "agents.";
    }
    std::string category() const override { return "context"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override;

    std::vector<std::unique_ptr<Capability>> capabilities() override;

    // What was loaded, from where, and how big — the same visibility the skills
    // surfaces give, for the same reason: a block the agent acts on should be
    // inspectable by the user. Empty when the project states nothing.
    std::vector<std::string> report_lines() const;

private:
    // Reads the workspace's AGENTS.md once and keeps it; returns "" when the
    // project has none.
    std::string load_once();

    mutable std::string block_;
    mutable bool loaded_ = false;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_PROJECT_INSTRUCTIONS_H

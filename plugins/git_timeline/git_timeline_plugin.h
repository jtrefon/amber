#ifndef AGENT_PLUGINS_GIT_TIMELINE_H
#define AGENT_PLUGINS_GIT_TIMELINE_H

// The timeline playbook: snapshots, the indexed subject, and the history moves
// that find and undo them.
//
// A skill, not resident prose: the coding domain has 110 lines of resident
// budget, and this depth belongs in the activation tier (read_skill). Shipped
// compiled in, so a fresh project has it from the first turn and it does not
// depend on the coding plugin's state.

#include "agent/plugin_core.h"

#include <string>
#include <vector>

namespace agent::plugins {

// The rendered skill body. Exposed for tests: the index grammar it teaches is
// what the resident prose and the git-workflow spec both point at.
std::string render_git_timeline_skill();

class GitTimelinePlugin : public IPlugin {
public:
    std::string id() const override { return "git-timeline"; }
    std::string version() const override { return "0.1.0"; }
    std::string name() const override { return "Git timeline"; }
    std::string description() const override {
        return "Snapshots with an indexed subject, and how to scan, pinpoint and undo them.";
    }
    std::string category() const override { return plugin_category::kDocs; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_GIT_TIMELINE_H

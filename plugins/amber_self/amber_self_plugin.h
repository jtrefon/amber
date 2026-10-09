
#ifndef AGENT_PLUGINS_AMBER_SELF_H
#define AGENT_PLUGINS_AMBER_SELF_H

// The self-knowledge plugin: it registers the `amber-config` skill, which
// tells the agent where amber keeps its own files and what changes each one.
//
// A skill rather than a system-prompt paragraph because of progressive
// disclosure: the discovery slot carries one line per turn, and the full
// layout loads only when the agent asks for it (read_skill). A skill rather
// than a SKILL.md file because this is the harness describing itself: the body
// is rendered from agent::config_paths(), the same table /get config paths
// prints, so the two cannot disagree, and there is nothing on disk for a
// stray edit to break.

#include "agent/plugin_core.h"

#include <string>
#include <vector>

namespace agent::plugins {

// The rendered body of the amber-config skill. Exposed for tests: the
// documented paths and the resolved ones must be the same paths.
std::string render_amber_config_skill();

class AmberSelfPlugin : public IPlugin {
public:
    std::string id() const override { return "amber-self"; }
    std::string version() const override { return "0.1.0"; }
    std::string name() const override { return "Amber self-knowledge"; }
    std::string description() const override {
        return "Teaches the agent where amber keeps its own configuration.";
    }
    std::string category() const override { return plugin_category::kDocs; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_AMBER_SELF_H

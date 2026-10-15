#ifndef AGENT_PLUGINS_PROJECT_INDEX_H
#define AGENT_PLUGINS_PROJECT_INDEX_H

#include "agent/plugin_core.h"

#include <string>
#include <vector>

namespace agent::plugins {

// The project index: amber's own notes about the project it is working in --
// how it runs, where its parts and artefacts live, which library entries apply
// -- as a session-fixed system block.
//
// The problem it removes: every session used to rededuce facts the project
// already knows -- that the work runs in docker compose, which container is
// shared with another project, which stack is in play. AGENTS.md covers what a
// project *declares*; nothing covered the structural facts amber derives and
// the user confirms, and nothing kept them across sessions.
//
// The file is read once and cached: the block rides in the system prompt every
// turn, so a per-render read would move the KV prefix whenever anything on disk
// changed. The file itself is the durable record -- tools write it immediately
// -- and the session's render is a cache of it that only activation, an
// explicit refresh, or a compaction updates. Disabling the plugin removes the
// block.
class ProjectIndexPlugin : public IPlugin {
public:
    std::string id() const override { return "project_index"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Project index"; }
    std::string description() const override {
        return "Amber's own notes about this project: how it runs, where things live, which "
               "library entries apply.";
    }
    std::string category() const override { return "domain"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override;

    std::vector<std::unique_ptr<Capability>> capabilities() override;

    // Drop the cache so the next render reads the file again. The session's
    // render is frozen otherwise; this is what an explicit refresh calls.
    void refresh();

    // The provenance hash of one file, 8 hex characters. Public because the
    // index is written with it -- a recorded hash is what a later read checks
    // against -- and because anyone should be able to recompute what is
    // checked. Empty when the file cannot be read (which a reader treats as
    // "changed", never as "fine").
    static std::string hash_file(const std::string& path);

    // The file this plugin reads, or empty when the workspace has no state
    // root. Public so the tools that write the index and the tests that place
    // one agree with the reader instead of reconstructing the path.
    static std::string file_path();

private:
    // Reads the index once and keeps the rendered block; "" when the project
    // has none.
    std::string load_once();

    mutable std::string block_;
    mutable bool loaded_ = false;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_PROJECT_INDEX_H


#ifndef AGENT_PLUGINS_CODING_H
#define AGENT_PLUGINS_CODING_H

#include "agent/plugin_core.h"

#include <vector>

namespace agent::plugins {

// The coding domain: the workflow and engineering discipline a software task
// needs, as two System blocks.
//
// The harness itself stays generic — identity, the tool protocol, the policy
// layer, the project's own AGENTS.md — and an activity arrives as a plugin that
// can be switched off. Coding ships enabled, because that is where most work
// happens and where the previous behaviour lives (core used to load the git
// prose unconditionally); a research or operations session turns it off and
// pays nothing for it.
class CodingPlugin : public IPlugin {
public:
    std::string id() const override { return "coding"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Coding"; }
    std::string description() const override {
        return "Software workflow and engineering discipline: version control, edit discipline, "
               "design principles, tests.";
    }
    std::string category() const override { return "domain"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_CODING_H

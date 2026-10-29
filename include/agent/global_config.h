#ifndef AGENT_GLOBAL_CONFIG_H
#define AGENT_GLOBAL_CONFIG_H

#include "agent/config.h"

namespace agent {

// The user's global config as it was read, for callers that need the raw file
// after the selection has been applied (the explicit model outranks a project
// default, so it is applied later in the layering order).
struct GlobalConfig {
    Config cfg;
    bool loaded = false;
};

// Apply the selection the user made in the global config: the provider endpoint
// and key (through the provider domain), their explicit context window, and the
// remembered model where the provider domain does not cover it. This is the
// configuration every host runs with, so a live benchmark calling it measures
// the setup the user actually has. Returns the raw file for the later model
// step; `loaded` is false when there is no global config, and nothing is
// imposed then.
GlobalConfig apply_global_selection(Config& cfg);

// A model saved to the global config (e.g. the TUI's /set model) outranks a
// project default, so it is applied after the project layer.
void apply_global_model(Config& cfg, const GlobalConfig& global);

} // namespace agent

#endif // AGENT_GLOBAL_CONFIG_H

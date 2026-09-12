#pragma once

#include "action_registry.h"
#include "setting_registry.h"

#include <agent/extensions.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace tui {

// Merge every plugin command contribution into `settings` and bind each
// executable leaf into `actions`.
//
// `entries` is read at dispatch time, so a disable/re-enable always resolves
// the live handler rather than a captured one. `output` receives the text a
// handler returns; empty output is dropped. Returns the number of actions
// bound.
//
// Kept free of `Tui` so the binding is unit-testable: the host passes its own
// settings registry, action registry and output sink.
std::size_t install_plugin_commands(
    const std::function<const std::vector<agent::CommandRegistry::Entry>&()>& entries,
    SettingRegistry& settings, ActionRegistry& actions,
    const std::function<void(const std::string&)>& output);

} // namespace tui

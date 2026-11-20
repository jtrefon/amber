#ifndef AMBER_TUI_PROVIDER_FORM_H
#define AMBER_TUI_PROVIDER_FORM_H

#include <string>

#include "agent/config.h"

namespace tui {

// Provider edit form (Server URL / API Key / Model). Returns true when the
// user saved, false on cancel. The context window is a deliberate pin, set
// with /set model context, not a provider-form field (a provider default that
// an existing pin outranks was the bug in #290).
//
// Shared by /provider, the settings screen and /profile add: all three edit a
// connection, and a second field list would drift from this one.
bool edit_provider_form(agent::Config& cfg, const std::string& title);

} // namespace tui

#endif // AMBER_TUI_PROVIDER_FORM_H

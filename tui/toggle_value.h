#ifndef AMBER_TUI_TOGGLE_VALUE_H
#define AMBER_TUI_TOGGLE_VALUE_H

#include <string>

// L1 (pure): the on|off|toggle value shape every boolean setting takes.
namespace tui {

// Whether v is one of the three accepted spellings. Callers that must reject a typo
// check this first; parse_toggle() alone would silently read anything else as "off".
bool valid_toggle(const std::string& v);

// Resolve v against the current value. An unrecognised spelling is "off", which is
// what the copies this replaced each did.
bool parse_toggle(const std::string& v, bool current);

} // namespace tui

#endif

#ifndef AMBER_TUI_PANEL_VIEW_H
#define AMBER_TUI_PANEL_VIEW_H

#include <string>

namespace agent {
class PanelRegistry;
}

namespace tui {

// Show a contributed panel full-screen, scrolling its lines and offering
// cycling between panels. Blocks until the user closes it (Esc/q), like the
// other modal views.
//
// The panel only supplies text and (optionally) key handling; framing,
// scrolling, cycling and closing belong to the host, so a plugin never learns
// what a window is. Returns the id of the panel shown, or "" when none exist.
std::string panel_view(const agent::PanelRegistry& panels, const std::string& start_id);

} // namespace tui

#endif // AMBER_TUI_PANEL_VIEW_H

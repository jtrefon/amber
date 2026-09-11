#ifndef AMBER_TUI_WINDOW_OPS_H
#define AMBER_TUI_WINDOW_OPS_H

#include <cstddef>
#include <string>
#include "tui/window_ops_port.h"

namespace tui {

class WindowManager;

// Result of a window operation: success/failure + a user-facing message
// (empty on success, usage/error text on failure).
struct WindowOpResult {
    bool ok = false;
    std::string msg;
};

// Use-case layer for window operations. Validates and mutates WindowManager
// state, then calls WindowOpsPort hooks for side effects (lazy load,
// autosave, redraw, status). Both the KeyBinder (hotkeys) and
// SlashDispatcher (/window commands) converge here — single implementation,
// two trigger paths. No ncurses dependency.
class WindowOps {
public:
    WindowOps(WindowManager& wm, WindowOpsPort& port);
    WindowOpResult switch_to(size_t idx);
    WindowOpResult new_window(const std::string& title);
    WindowOpResult close_window();
    std::string list_windows() const;
    WindowOpResult rename_window(const std::string& name);
    // One-based user index (as shown in /window list and Alt+number).
    WindowOpResult set_window(size_t one_based_idx);

private:
    WindowManager& wm_;
    WindowOpsPort& port_;
};

} // namespace tui

#endif // AMBER_TUI_WINDOW_OPS_H

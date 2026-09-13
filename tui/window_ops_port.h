#ifndef AMBER_TUI_WINDOW_OPS_PORT_H
#define AMBER_TUI_WINDOW_OPS_PORT_H

#include <string>

namespace tui {

// Port: side-effect hooks for window operations. WindowOps (the use-case
// layer) validates and mutates WindowManager state, then calls these hooks
// to perform UI/session side effects (lazy load, autosave, redraw). This
// keeps WindowOps free of ncurses and Tui dependencies.
class WindowOpsPort {
public:
    virtual ~WindowOpsPort() = default;
    // Per-window busy: true when the agent of the window at `idx` is
    // mid-run. Switching is never gated on it; closing is gated on the
    // TARGET window's flag only (a running sibling must not block).
    virtual bool is_busy(size_t idx) const = 0;
    virtual void on_switch() = 0;                    // lazy_load (never clears sibling state)
    virtual void on_close() = 0;                     // autosave
    virtual void redraw() = 0;                       // draw()
    virtual void status(const std::string& msg) = 0; // status bar feedback
};

} // namespace tui

#endif // AMBER_TUI_WINDOW_OPS_PORT_H

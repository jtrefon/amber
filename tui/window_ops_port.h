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
    virtual bool is_busy() const = 0;
    virtual void on_switch() = 0;   // lazy_load + clear pending tools
    virtual void on_close() = 0;    // autosave
    virtual void redraw() = 0;      // draw()
    virtual void status(const std::string& msg) = 0;  // status bar feedback
};

} // namespace tui

#endif // AMBER_TUI_WINDOW_OPS_PORT_H

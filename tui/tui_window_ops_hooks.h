#ifndef AMBER_TUI_TUI_WINDOW_OPS_HOOKS_H
#define AMBER_TUI_TUI_WINDOW_OPS_HOOKS_H

#include "tui/window_ops_port.h"

namespace tui {

class Tui;

// Adapter: implements WindowOpsPort using Tui internals. Bridges the
// domain-core WindowOps to the ncurses/agent side effects (busy check,
// lazy load, autosave, redraw, status bar). This is the only adapter
// that touches Tui private members for window operations.
class TuiWindowOpsHooks : public WindowOpsPort {
public:
    explicit TuiWindowOpsHooks(Tui& tui);
    bool is_busy() const override;
    void on_switch() override;
    void on_close() override;
    void redraw() override;
    void status(const std::string& msg) override;

private:
    Tui& tui_;
};

} // namespace tui

#endif // AMBER_TUI_TUI_WINDOW_OPS_HOOKS_H

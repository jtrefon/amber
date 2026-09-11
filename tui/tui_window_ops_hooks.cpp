#include "tui/tui_window_ops_hooks.h"
#include "tui/tui.h"
#include "tui/widgets.h"

namespace tui {

TuiWindowOpsHooks::TuiWindowOpsHooks(Tui& tui) : tui_(tui) {}

bool TuiWindowOpsHooks::is_busy() const {
    return tui_.router_->busy();
}

void TuiWindowOpsHooks::on_switch() {
    tui_.router_->pending_tools().clear();
    tui_.lazy_load_active();
}

void TuiWindowOpsHooks::on_close() {
    tui_.autosave();
    tui_.router_->pending_tools().clear();
}

void TuiWindowOpsHooks::redraw() {
    tui_.draw();
}

void TuiWindowOpsHooks::status(const std::string& msg) {
    tui_.append_line(P_STATUS, msg);
}

} // namespace tui

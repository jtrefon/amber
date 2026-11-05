#include "tui/tui_window_ops_hooks.h"

#include "tui/task_runtime.h"
#include "tui/watch_cancel.h"
#include "tui/tui.h"
#include "tui/widgets.h"

#include <algorithm>

namespace tui {

TuiWindowOpsHooks::TuiWindowOpsHooks(Tui& tui) : tui_(tui) {}

bool TuiWindowOpsHooks::is_busy(size_t idx) const {
    auto& all = tui_.window_manager_->all();
    if (idx >= all.size() || !all[idx])
        return false;
    return tui_.runs_.busy(all[idx]->id);
}

void TuiWindowOpsHooks::on_switch() {
    // Pending tool lines are window-stamped — leave them alone: a
    // background window's spinner must close in place when its result
    // lands, not be dropped because the user looked at another window.
    tui_.lazy_load_active();
    // The task feed is per-window: switching owners changes which ids are
    // worth completing under /tasks.
    tui_.refresh_task_feed();
}

void TuiWindowOpsHooks::on_close() {
    // on_close fires before the window is erased, so win() is the closing one.
    if (tui_.win().kind == Window::Kind::Resource) {
        tui_.draw();
        return;
    }
    size_t closing_id = tui_.win().id;
    // A task belongs to the window that started it; stop its non-terminal work
    // before the workspace it runs in is released.
    if (tui_.tasks_) {
        const std::string owner = tui_.win().owner_id;
        for (const auto& record : tui_.tasks_->list(owner)) {
            if (agent::task_state_terminal(record.state))
                continue;
            agent::CancellationToken token;
            tui_.tasks_->cancel(record.id, "window closed", token);
        }
    }
    cancel_owned_watches(tui_.plugin_runtime_, tui_.win().owner_id);
    // A completion already queued for this owner must not wake a window that
    // no longer exists; the watch producer is abandoned, not acked.
    tui_.task_runtime_->inbox.drop(tui_.win().owner_id);
    tui_.autosave();
    // The workspace is released while the window still exists (autosave just
    // ran, so its conversation is safe), and its notice is flushed on the next
    // redraw — appending now would write into the window being erased.
    tui_.release_window_workspace(tui_.win());
    // Drop only ITS spinner rows; a sibling's must survive.
    auto& pts = tui_.router_->pending_tools();
    pts.erase(std::remove_if(
                  pts.begin(), pts.end(),
                  [closing_id](const PendingToolLine& pt) { return pt.window_id == closing_id; }),
              pts.end());
    tui_.runs_.erase(closing_id); // safe: close is rejected while busy
}

void TuiWindowOpsHooks::redraw() {
    tui_.flush_workspace_notice();
    tui_.draw();
}

void TuiWindowOpsHooks::status(const std::string& msg) {
    tui_.append_line(P_STATUS, msg);
}

} // namespace tui

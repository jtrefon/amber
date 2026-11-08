#ifndef AMBER_TUI_WORKER_TARGET_H
#define AMBER_TUI_WORKER_TARGET_H

#include "tui/window.h"
#include "tui/window_manager.h"

#include <cstddef>
#include <string>

namespace tui {

// Where a worker's blocking question goes. The owner window is resolved by its
// stable owner id (not by the active index), because a worker can outlive the
// turn that spawned it. An inactive owner does not move focus: the dialog goes
// to the active window and the text names the owner. A missing owner means the
// window closed, and the caller must fail closed.
struct WorkerTarget {
    bool ok = false;
    std::size_t window_id = std::string::npos; // npos: the active window at drain
    std::string owner_title;
};

inline WorkerTarget resolve_worker_target(WindowManager& windows, const std::string& owner_id) {
    Window* owner = windows.by_owner_id(owner_id);
    if (!owner || windows.count() == 0)
        return {};
    WorkerTarget target;
    target.ok = true;
    target.owner_title = owner->title;
    // Only an already-active owner is addressed directly. Otherwise leave the
    // event to the active window at drain time, so a tab switch between asking
    // and painting cannot strand the dialog on a window nobody is looking at.
    const Window& active = windows.win();
    if (active.id == owner->id)
        target.window_id = owner->id;
    return target;
}

} // namespace tui

#endif // AMBER_TUI_WORKER_TARGET_H

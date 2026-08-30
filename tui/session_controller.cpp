#include "session_controller.h"
#include "tui.h"

#include <agent/workspace.h>

namespace tui {

SessionController::SessionController(Tui& tui) : tui_(tui) {}

agent::WorkspaceState SessionController::load_workspace() {
    return store_.load_workspace();
}

} // namespace tui
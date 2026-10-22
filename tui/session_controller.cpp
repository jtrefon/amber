#include "session_controller.h"
#include "tui.h"

#include <agent/workspace.h>

namespace tui {

SessionController::SessionController(Tui& tui) : tui_(tui) {}

agent::SessionStore& SessionController::store() {
    return store_for(tui_.win().project_root);
}

agent::SessionStore& SessionController::store_for(const std::string& project_root) {
    const std::string root = project_root.empty() ? agent::Workspace::project_root() : project_root;
    auto it = stores_.find(root);
    if (it != stores_.end())
        return it->second;
    agent::ScopedProjectRoot guard(root);
    agent::Workspace::adopt_legacy_state("sessions");
    auto inserted = stores_.try_emplace(root, agent::Workspace::state_dir() + "/sessions");
    return inserted.first->second;
}

std::string SessionController::settings_path() const {
    const std::string root = tui_.win().project_root.empty() ? agent::Workspace::project_root()
                                                             : tui_.win().project_root;
    return agent::Workspace::settings_path_for(root);
}

agent::WorkspaceState SessionController::load_workspace() {
    return store_for(agent::Workspace::project_root()).load_workspace();
}

} // namespace tui
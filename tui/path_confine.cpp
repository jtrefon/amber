
#include "path_confine.h"
#include "agent/workspace.h"

namespace tui {

bool confine_path(const std::string& input, std::string& resolved,
                  std::string& err) {
    return agent::Workspace::confine(input, resolved, err);
}

} // namespace tui

#include "widgets.h"

#include "tui/modal_host.h"

#include <string>
#include <vector>

namespace tui {

int menu_select(const std::string& title, const std::vector<std::string>& choices) {
    return ModalHost{}.choose(title, choices);
}

} // namespace tui

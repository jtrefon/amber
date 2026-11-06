#include "widgets.h"

#include "tui/modal_host.h"

#include <string>
#include <vector>

namespace tui {

void info_dialog(const std::string& title, const std::vector<std::string>& rows) {
    ModalHost{}.info(title, rows);
}

} // namespace tui

#include "tui/list_panel.h"

#include "tui/modal_host.h"

#include <utility>

namespace tui {

ListPanel::ListPanel(std::string title, std::vector<std::string> items)
    : title_(std::move(title)), items_(std::move(items)) {}

int ListPanel::run() {
    return ModalHost{}.choose(title_, items_);
}

} // namespace tui

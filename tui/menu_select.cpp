// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "widgets.h"
#include "tui/list_panel.h"

namespace tui {

int menu_select(const std::string& title, const std::vector<std::string>& choices) {
    ModalScope scope;
    curs_set(0);

    ListPanel panel(title, choices);
    return panel.run();
}

} // namespace tui

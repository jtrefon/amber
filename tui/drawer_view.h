#pragma once

#include "tui/drawer_tabs.h"
#include "tui/surface.h"

#include <string>
#include <vector>

namespace tui {

// The command drawer / model picker as one view-model. The rows are already
// windowed by `drawer_window`, so the painter only has to place them: the header
// strip, the provider tabs on its right, and the selected row. It never computes
// which rows exist or which one is selected -- those are the command tree's and
// the input loop's decisions, tested without a terminal.
struct DrawerView {
    std::string header;
    DrawerTabStrip tabs;
    std::vector<std::string> rows; // visible window only
    int selected = 0;              // index into `rows`
};

// Paint the drawer into a self-contained surface of `width` columns and
// 1 + rows.size() rows. The caller blits it at the drawer's top row.
Surface paint_drawer(const DrawerView& view, int width);

} // namespace tui

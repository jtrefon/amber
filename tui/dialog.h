// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_DIALOG_H
#define AMBER_TUI_DIALOG_H

#include "tui/panel.h"

namespace tui {

// Dialog is a thin wrapper around Panel providing the same API as the
// legacy class for backwards compatibility with form_edit, info_dialog,
// and session_browser. New code should use Panel directly.
class Dialog : private Panel {
public:
    Dialog(int h, int w, const std::string& title);
    ~Dialog() = default;

    using Panel::win;
    using Panel::content;
    using Panel::rows;
    using Panel::cols;
    using Panel::show;
    using Panel::hide;
};

} // namespace tui

#endif // AMBER_TUI_DIALOG_H

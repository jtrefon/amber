#ifndef AMBER_TUI_MODAL_PORT_H
#define AMBER_TUI_MODAL_PORT_H

#include <string>
#include <vector>

namespace tui {

// Port: modal dialogs (menu select, info popup). The domain core uses this
// to display modals without depending on ncurses panels.
class ModalPort {
public:
    virtual ~ModalPort() = default;
    virtual int menu_select(const std::string& title,
                            const std::vector<std::string>& items) = 0;
    virtual void info_dialog(const std::string& title,
                             const std::vector<std::string>& lines) = 0;
    virtual void redraw_after_modal() = 0;
};

} // namespace tui

#endif // AMBER_TUI_MODAL_PORT_H

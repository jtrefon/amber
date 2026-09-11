#include "tui/window_ops.h"
#include "tui/window.h"
#include "tui/window_manager.h"

#include <sstream>

namespace tui {

WindowOps::WindowOps(WindowManager& wm, WindowOpsPort& port)
    : wm_(wm), port_(port) {}

WindowOpResult WindowOps::switch_to(size_t idx) {
    if (!wm_.valid_index(idx))
        return {false, "window index out of range"};
    if (idx == wm_.active())
        return {true, ""};
    if (port_.is_busy())
        return {false, "cannot switch windows while agent is busy"};
    wm_.set_active(idx);
    port_.on_switch();
    port_.redraw();
    return {true, ""};
}

WindowOpResult WindowOps::new_window(const std::string& title) {
    wm_.new_window(title);
    port_.redraw();
    return {true, ""};
}

WindowOpResult WindowOps::close_window() {
    if (wm_.count() <= 1)
        return {false, "cannot close the last window"};
    if (port_.is_busy())
        return {false, "cannot close window while agent is busy"};
    port_.on_close();
    wm_.all().erase(wm_.all().begin() + wm_.active());
    if (wm_.active() >= wm_.count())
        wm_.set_active(wm_.count() - 1);
    port_.redraw();
    return {true, ""};
}

std::string WindowOps::list_windows() const {
    std::ostringstream os;
    for (size_t i = 0; i < wm_.count(); ++i) {
        if (i > 0) os << "  ";
        os << (i + 1) << ":";
        if (i == wm_.active()) os << "*";
        os << wm_.all()[i]->title;
    }
    return os.str();
}

WindowOpResult WindowOps::rename_window(const std::string& name) {
    if (name.empty())
        return {false, "window name cannot be empty"};
    wm_.win().title = name;
    port_.redraw();
    return {true, ""};
}

WindowOpResult WindowOps::set_window(size_t one_based_idx) {
    if (one_based_idx == 0)
        return {false, "window numbers start at 1"};
    return switch_to(one_based_idx - 1);
}

} // namespace tui

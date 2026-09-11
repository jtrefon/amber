#include "tui/window_ops.h"
#include "tui/window_manager.h"

namespace tui {

WindowOps::WindowOps(WindowManager& wm, WindowOpsPort& port)
    : wm_(wm), port_(port) {}

// RED stubs: all operations fail. Green implementation will validate,
// mutate WindowManager, and call port hooks.

WindowOpResult WindowOps::switch_to(size_t idx) {
    (void)idx;
    return {false, "not implemented"};
}

WindowOpResult WindowOps::new_window(const std::string& title) {
    (void)title;
    return {false, "not implemented"};
}

WindowOpResult WindowOps::close_window() {
    return {false, "not implemented"};
}

std::string WindowOps::list_windows() const {
    return "not implemented";
}

WindowOpResult WindowOps::rename_window(const std::string& name) {
    (void)name;
    return {false, "not implemented"};
}

WindowOpResult WindowOps::set_window(size_t one_based_idx) {
    (void)one_based_idx;
    return {false, "not implemented"};
}

} // namespace tui

#include "tui/widgets.h"

namespace tui {

namespace {
bool* g_modal_flag = nullptr;
} // namespace

void set_modal_flag(bool* flag) {
    g_modal_flag = flag;
}

ModalScope::ModalScope() {
    if (g_modal_flag)
        *g_modal_flag = true;
}

ModalScope::~ModalScope() {
    if (g_modal_flag)
        *g_modal_flag = false;
}

} // namespace tui

#include "tui/widgets.h"

namespace tui {

namespace {
int g_modal_depth = 0;
} // namespace

bool modal_open() noexcept {
    return g_modal_depth > 0;
}

ModalScope::ModalScope() {
    ++g_modal_depth;
}

ModalScope::~ModalScope() {
    if (g_modal_depth > 0)
        --g_modal_depth;
}

} // namespace tui

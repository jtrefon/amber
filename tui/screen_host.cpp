#include "tui/screen_view.h"

#include "tui/keys.h"
#include "tui/terminal.h"
#include "tui/widgets.h"

#include <utility>

namespace tui {

void ScreenHost::run(ScreenView& view, const KeyHandler& handler, int poll_ms, const Tick& tick) {
    ModalScope modal;
    Terminal terminal;
    terminal.adopt();
    struct Restore {
        Terminal& terminal;
        ~Restore() { terminal.set_timeout(kTickTimeoutMs); }
    } restore{terminal};

    for (;;) {
        const PaintedScreen painted = paint_screen(view, terminal.size());
        terminal.overlay(painted.surface, 0, 0, painted.cursor_x, painted.cursor_y);
        terminal.set_timeout(poll_ms);
        const int key = terminal.read_key();
        if (key == keys::kResize)
            continue;
        if (key == keys::kNone) {
            if (tick)
                tick();
            continue;
        }
        if (handler(key))
            break;
    }
    terminal.close_overlay();
}

} // namespace tui

#include "tui/gallery_screen.h"

#include "tui/gallery.h"
#include "tui/keys.h"
#include "tui/terminal.h"

namespace tui {

int run_gallery_screen() {
    Terminal terminal;
    terminal.start();

    GlyphMode mode = GlyphMode::Unicode;
    ThemeKind kind = ThemeKind::Amber;
    terminal.set_theme(Theme::make(kind, mode));
    Surface surface(terminal.size().w, terminal.size().h);
    paint_gallery(surface, mode, kind);
    terminal.present(surface);

    for (;;) {
        const int key = terminal.read_key();
        if (key == 27 || key == 'q' || key == 'Q')
            break;
        if (key == keys::kResize) {
            terminal.clear_screen();
            const Size size = terminal.size();
            if (size.w != surface.width() || size.h != surface.height())
                surface = Surface(size.w, size.h);
        } else if (key == 'a' || key == 'A') {
            mode = mode == GlyphMode::Unicode ? GlyphMode::Ascii : GlyphMode::Unicode;
            terminal.set_theme(Theme::make(kind, mode));
        } else if (key == 't' || key == 'T') {
            kind = static_cast<ThemeKind>((static_cast<int>(kind) + 1) % 4);
            terminal.set_theme(Theme::make(kind, mode));
        } else {
            continue;
        }
        paint_gallery(surface, mode, kind);
        terminal.present(surface);
    }

    terminal.stop();
    return 0;
}

} // namespace tui

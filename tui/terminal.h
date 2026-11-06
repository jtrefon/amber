#ifndef AMBER_TUI_TERMINAL_H
#define AMBER_TUI_TERMINAL_H

// The terminal adapter: the one place the shell talks to ncurses. It owns
// initialisation, the screen size, a cell-grid blit and a blocking key read.
// The header is ncurses-free on purpose — this is the layer boundary the ratchet
// measures, and later screens depend on the interface, not on curses types.

#include "tui/geometry.h"
#include "tui/surface.h"

#include <memory>

namespace tui {

class Terminal {
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void start();
    void stop();

    Size size() const;

    // Repaint only the cells whose value changed since the last present, then
    // flush with one doupdate(). Commits the surface on success.
    void present(Surface& surface);

    // Erase the physical screen. A caller that replaces its surface (a resize)
    // uses this so cells with no damage cannot keep stale content.
    void clear_screen();

    // Blocking read. Returns the ncurses key code (see tui/keys.h) or
    // keys::kNone on a timeout/ERR.
    int read_key();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tui

#endif // AMBER_TUI_TERMINAL_H

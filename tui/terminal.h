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

    // The Tui already owns curses. A dialog host calls adopt() to register the
    // theme pairs without re-running initscr.
    void adopt();

    Size size() const;

    // A modal overlay: paint a whole surface at (x, y) over the current screen.
    // No diff: the surface is self-contained, and close_overlay() restores the
    // screen buffer that was underneath.
    void overlay(const Surface& surface, int x, int y, int cursor_x = -1, int cursor_y = -1);
    void close_overlay();

    // Write a self-contained surface at (x, y) into the shell's own screen
    // buffer, without refreshing. The caller's existing doupdate() presents it,
    // which is how the drawer joins the frame the rest of the shell painted.
    void blit(const Surface& surface, int x, int y);

    void set_timeout(int milliseconds);
    // One non-blocking read (the modal paste probe).
    int read_key_now();

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

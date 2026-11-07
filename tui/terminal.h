#ifndef AMBER_TUI_TERMINAL_H
#define AMBER_TUI_TERMINAL_H

// The terminal adapter: the one place the shell talks to ncurses. It owns
// initialisation, the screen size, a cell-grid blit and a blocking key read.
// The header is ncurses-free on purpose — this is the layer boundary the ratchet
// measures, and later screens depend on the interface, not on curses types.

#include "tui/geometry.h"
#include "tui/keys.h"
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

    // Physical cursor. The shell paints a whole frame and then asks for the
    // cursor it owns (the input line); a hidden cursor is the default.
    void place_cursor(int x, int y);
    void hide_cursor();

    void set_timeout(int milliseconds);
    // One non-blocking read (the modal paste probe).
    int read_key_now();

    // Repaint only the cells whose value changed since the last present, then
    // flush with one doupdate(). Commits the surface on success.
    void present(Surface& surface);

    // Erase the physical screen. A caller that replaces its surface (a resize)
    // uses this so cells with no damage cannot keep stale content.
    void clear_screen();

    // Read with the adapter's current timeout. Returns a key code from
    // tui/keys.h, or keys::kNone on a timeout.
    int read_key();
    // Read with a temporary timeout (the ESC/UTF-8 follow-up probes), restoring
    // the shell's tick timeout afterwards.
    int read_key_timeout(int milliseconds);
    // Put a key back when a probe finds it is not part of the sequence.
    void push_key(int key);
    bool read_mouse(keys::MouseEvent& event);

    // Mouse reporting is a terminal mode, not a key binding.
    void set_mouse(bool enabled);
    // Present what was written: one virtual-screen refresh and one doupdate().
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tui

#endif // AMBER_TUI_TERMINAL_H

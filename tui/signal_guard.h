#ifndef AMBER_TUI_SIGNAL_GUARD_H
#define AMBER_TUI_SIGNAL_GUARD_H

#include <csignal>
#include <termios.h>
#include <unistd.h>

namespace tui {

// Once-only shutdown flag set from an async-signal-safe context and consumed
// by the main event loop. The loop turns the flag into a graceful teardown
// (workspace save + endwin); the signal handler's alarm fallback guarantees
// the process still dies if the loop cannot run (e.g. blocked in a modal).
class SignalState {
public:
    void raise(int sig) noexcept {
        flag_ = 1;
        sig_ = sig;
    }
    bool consume() noexcept {
        if (!flag_)
            return false;
        flag_ = 0;
        return true;
    }
    // Signal number recorded by raise(); valid only after consume().
    int signal() const noexcept { return sig_; }

private:
    volatile std::sig_atomic_t flag_ = 0;
    volatile std::sig_atomic_t sig_ = 0;
};

// Captures the terminal's termios before initscr() switches it to raw mode.
// restore() is async-signal-safe (write + tcsetattr) so a signal handler, a
// crash, or an uncaught exception can always leave the user's shell usable —
// including the mouse reporting and alternate screen that initscr/mousemask
// turn on, which only endwin() undoes otherwise. Without that, a process that
// dies without reaching endwin() leaves the terminal reporting mouse events:
// drag-to-select stops selecting, and the scrollback is unreachable.
class TerminalGuard {
public:
    void capture() noexcept { valid_ = tcgetattr(STDIN_FILENO, &saved_) == 0; }
    // Called once curses has turned on modes that outlive the process.
    void arm() noexcept { armed_ = true; }
    bool armed() const noexcept { return armed_; }

    void restore() noexcept {
        if (armed_)
            write_modes_off();
        if (valid_)
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }

private:
    // The literal sequences ncurses would emit, written directly because none
    // of its functions are async-signal-safe.
    void write_modes_off() noexcept {
        static constexpr const char kModesOff[] =
            "\033[?1006l\033[?1003l\033[?1002l\033[?1000l" // mouse reporting
            "\033[?2004l"                                  // bracketed paste
            "\033[?1l\033>"                                // application keypad
            "\033[?25h"                                    // cursor visible
            "\033[0m"                                      // attributes off
            "\033[?1049l";                                 // leave alternate screen
        const ssize_t ignored = ::write(STDOUT_FILENO, kModesOff, sizeof(kModesOff) - 1);
        (void)ignored;
    }

    struct termios saved_ {};
    bool valid_ = false;
    bool armed_ = false;
};

} // namespace tui

#endif // AMBER_TUI_SIGNAL_GUARD_H

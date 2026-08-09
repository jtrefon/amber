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
        if (!flag_) return false;
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
// restore() is async-signal-safe (tcsetattr + a plain write) so a signal
// handler can always leave the user's shell usable, even when the app dies
// without endwin().
class TerminalGuard {
public:
    void capture() noexcept {
        valid_ = tcgetattr(STDIN_FILENO, &saved_) == 0;
    }
    void restore() noexcept {
        if (valid_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        static const char exit_alt_scroll[] = "\033[?1007l";
        (void)write(STDOUT_FILENO, exit_alt_scroll, sizeof(exit_alt_scroll) - 1);
    }

private:
    struct termios saved_{};
    bool valid_ = false;
};

} // namespace tui

#endif // AMBER_TUI_SIGNAL_GUARD_H

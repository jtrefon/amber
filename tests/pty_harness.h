#ifndef AMBER_TESTS_PTY_HARNESS_H
#define AMBER_TESTS_PTY_HARNESS_H

// A real terminal harness for the ncurses binary: a pty, a key writer, a screen
// reader and the quit-path invariants (exit-ready marker, no SIGKILL). Shared by
// the pty regression suite and the agent journeys.

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "agent/exit_ready.h"

namespace agent::test::pty {

namespace fs = std::filesystem;

// How long one pump slice waits for output.
constexpr int kPumpMs = 50;
// A modal dismissed with Esc has to redraw before Ctrl+C can reach the main
// loop. This is the cap for "a modal was open"; a session with no modal returns
// as soon as any output arrives, so it does not pay the full budget.
constexpr int kModalDismissCapMs = 1000;
// How long the app gets to finish its quit path. Override with
// AMBER_PTY_EXIT_TIMEOUT_MS (milliseconds) on a very slow runner.
constexpr int kExitTimeoutMs = 60000;

// Drop terminal control sequences so assertions can look at rendered text.
std::string strip_ansi(const std::string& in);

void record_killed_session(const std::string& name);
void record_missing_exit_ready(const std::string& name);

struct Tui {
    int master = -1;
    pid_t pid = -1;
    int exit_code = -1;
    std::string raw;
    std::string name; // scenario name, reported if the session is force-killed

    ~Tui() { stop(); }

    // Every scenario is a fresh process with a fresh inventory. The durable task
    // store would otherwise seed the previous scenario's completions into this
    // one, which is correct on a real restart and noise in a scenario harness.
    // A test that exercises a restart opts out and keeps the state.
    static void reset_task_inventory(const std::string& workspace) {
        std::error_code ec;
        const fs::path projects = fs::path(workspace) / "state" / "amber" / "projects";
        for (const auto& project : fs::directory_iterator(projects, ec)) {
            if (ec)
                break;
            std::error_code ignored;
            if (project.is_directory(ignored))
                fs::remove_all(project.path() / "tasks", ignored);
        }
        std::error_code ignored;
        fs::remove_all(fs::path(workspace) / "cache" / "amber" / "tasks", ignored);
    }

    bool start(const std::string& binary, const std::string& workspace,
               const std::string& path_prefix = "", int rows = 0, int cols = 0,
               const std::vector<std::string>& extra_args = {}, bool preserve_task_state = false,
               bool keep_draft = false) {
        if (!preserve_task_state)
            reset_task_inventory(workspace);
        // The composer keeps an unsent draft per workspace, and every test in
        // this file shares one fixture workspace. Start each one clean unless
        // the test is deliberately exercising restore.
        if (!keep_draft) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(workspace) / ".amber" / "draft", ec);
        }
        std::vector<std::string> argv_strings;
        argv_strings.push_back(binary);
        argv_strings.insert(argv_strings.end(), extra_args.begin(), extra_args.end());
        std::vector<char*> argv;
        argv.reserve(argv_strings.size() + 1);
        for (auto& s : argv_strings)
            argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
            return false;
        const char* slave_name = ptsname(master);
        if (!slave_name)
            return false;

        pid = fork();
        if (pid < 0)
            return false;
        if (pid == 0) {
            int slave = open(slave_name, O_RDWR);
            if (slave < 0)
                _exit(127);
            setsid();
            ioctl(slave, TIOCSCTTY, 0);
            dup2(slave, STDIN_FILENO);
            dup2(slave, STDOUT_FILENO);
            dup2(slave, STDERR_FILENO);
            if (slave > STDERR_FILENO)
                close(slave);
            const char* term_override = std::getenv("AMBER_PTY_TERM");
            setenv("TERM", (term_override && *term_override) ? term_override : "xterm-256color", 1);
            // Ask for the exit announcement. The path is not passed: the app
            // derives it from its own state dir and pid, and the harness below
            // computes the same one.
            setenv("AMBER_EXIT_READY", "1", 1);
            // Deliver a lone Esc promptly: ncurses otherwise waits ESCDELAY
            // (1 s by default) to decide it is not an escape-sequence prefix,
            // which would stall the teardown below.
            setenv("ESCDELAY", "25", 1);
            // A test whose output needs more room than the 80x24 default asks
            // for it here. The pty has no winsize of its own, and ncurses reads
            // LINES/COLUMNS at initscr (the app never calls use_env(FALSE)), so
            // the environment is what gives it a size.
            setenv("LINES", std::to_string(rows > 0 ? rows : 24).c_str(), 1);
            setenv("COLUMNS", std::to_string(cols > 0 ? cols : 80).c_str(), 1);
            if (!path_prefix.empty()) {
                std::string path = path_prefix;
                if (const char* inherited = getenv("PATH"))
                    path += std::string(":") + inherited;
                setenv("PATH", path.c_str(), 1);
            }
            if (chdir(workspace.c_str()) != 0) {
                std::fprintf(stderr, "pty child: chdir(%s): %s\n", workspace.c_str(),
                             std::strerror(errno));
                _exit(127);
            }
            execv(argv[0], argv.data());
            std::fprintf(stderr, "pty child: exec(%s): %s\n", binary.c_str(), std::strerror(errno));
            _exit(127);
        }
        int flags = fcntl(master, F_GETFL, 0);
        fcntl(master, F_SETFL, flags | O_NONBLOCK);
        return true;
    }

    // Read whatever the app produces for the whole window, then return. The
    // window is wall-clock, not "until the first idle slice": callers use this
    // as a delay (a key gap, a settle before the next assertion), and a pump
    // that returns the moment the app pauses turns every `pump(1200) // longer
    // than ESCDELAY` into a ~50 ms gap, which is how a lone ESC ends up folded
    // into the following key on a slow runner. It also makes `wait_for`'s cap
    // a real budget: its loop advances by kPumpMs per iteration, so an
    // iteration must cost kPumpMs.
    bool pump(int ms) {
        bool got = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        do {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(master, &rfds);
            timeval tv{0, static_cast<suseconds_t>(kPumpMs) * 1000};
            int ready = select(master + 1, &rfds, nullptr, nullptr, &tv);
            if (ready <= 0)
                continue;
            char buf[8192];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                raw.append(buf, static_cast<size_t>(n));
                got = true;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        } while (std::chrono::steady_clock::now() < deadline);
        return got;
    }

    // Last rendered text, for diagnosing a failure in CI.
    void dump() const {
        std::string t = text();
        std::cerr << "--- amber output (last 800 chars) ---\n"
                  << t.substr(t.size() > 800 ? t.size() - 800 : 0) << "\n--- end ---\n";
    }

    bool wait_for(const std::string& marker, int cap_ms) {
        for (int waited = 0; waited < cap_ms; waited += kPumpMs) {
            pump(kPumpMs);
            if (text().find(marker) != std::string::npos)
                return true;
        }
        return false;
    }

    // Wait for the app to exit through its own quit path. Reaping here is what
    // keeps stop() from reporting the session as force-killed, so a test can
    // assert the quit itself rather than only that no key was lost.
    bool wait_exit(int cap_ms) {
        for (int waited = 0; waited < cap_ms && pid > 0; waited += kPumpMs) {
            pump(kPumpMs);
            if (reap_if_exited())
                return true;
        }
        return pid < 0;
    }

    // Reap without blocking. A child that exited counts as reaped even when
    // waitpid reports the error: there is nothing left to wait for.
    bool reap_if_exited() {
        if (pid <= 0)
            return true;
        int status = 0;
        const pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            pid = -1;
        } else if (reaped < 0) {
            pid = -1;
        }
        return pid < 0;
    }

    static int exit_timeout_ms() {
        const char* raw = std::getenv("AMBER_PTY_EXIT_TIMEOUT_MS");
        if (!raw || !*raw)
            return kExitTimeoutMs;
        char* end = nullptr;
        errno = 0;
        const long parsed = std::strtol(raw, &end, 10);
        if (errno != 0 || end == raw || parsed <= 0)
            return kExitTimeoutMs;
        return static_cast<int>(std::min<long>(parsed, 600000));
    }

    // A test may end inside a modal (e.g. the error journal or the session
    // browser), which reads its own window and would swallow Ctrl+C. Esc closes
    // it. Wait for the redraw that follows instead of sleeping a fixed 0.8 s: a
    // loaded runner gets as long as it needs, and a fast one does not stall.
    void dismiss_modal() {
        const size_t mark = raw.size();
        write_raw("\x1b");
        for (int waited = 0; waited < kModalDismissCapMs && raw.size() == mark; waited += kPumpMs)
            pump(kPumpMs);
        // The dismissal's own redraw may still be in flight; one more slice
        // lets it finish without making the common case wait for a timeout.
        pump(kPumpMs);
    }

    // Ask the app to quit, then drain until it is gone. Draining is what lets a
    // blocked-on-write app read the quit key at all, and the reap is the event
    // that ends the wait: atexit runs after it, which is where gcov flushes. The
    // chord is repeated while waiting because a modal that was mid-transition
    // when Ctrl+C arrived swallows it, and a second Esc closes it -- the race the
    // old single-shot fixed budget lost on a loaded runner.
    void wait_for_quit() {
        const int timeout_ms = exit_timeout_ms();
        int next_chord = 15000;
        for (int waited = 0; waited < timeout_ms && pid > 0; waited += kPumpMs) {
            pump(kPumpMs);
            if (reap_if_exited())
                return;
            if (waited >= next_chord) {
                next_chord = waited + 15000;
                dismiss_modal();
                write_raw("\x03");
            }
        }
    }

    // The app writes this after the terminal is restored and the sessions are
    // saved. Its presence is evidence the quit path ran; a session that exits
    // without it died inside its own teardown.
    bool exit_ready_seen() const { return raw.find(agent::kExitReadyMarker) != std::string::npos; }

    std::string text() const { return strip_ansi(raw); }

    // A real terminal tracks the cursor-key mode the app requested: ncurses emits
    // smkx when it is about to read a keypad-enabled window and rmkx otherwise.
    bool application_cursor_mode() const {
        size_t smkx = raw.rfind("\x1b[?1h");
        size_t rmkx = raw.rfind("\x1b[?1l");
        return smkx != std::string::npos && (rmkx == std::string::npos || smkx > rmkx);
    }

    void write_raw(const std::string& bytes) {
        ssize_t ignored = ::write(master, bytes.data(), bytes.size());
        (void)ignored;
    }

    void send(const std::string& text) { write_raw(text); }

    // Arrow sequences differ between the two cursor-key modes: the app asks for
    // application mode with smkx, and a real terminal answers with SS3.
    std::string arrow_sequence(char final_byte) const {
        std::string seq = application_cursor_mode() ? "\x1bO" : "\x1b[";
        seq.push_back(final_byte);
        return seq;
    }

    // What a modal would leave in the prompt if it failed to decode the key: the
    // sequence minus the ESC that it consumed as "cancel".
    std::string arrow_tail(char final_byte) const { return arrow_sequence(final_byte).substr(1); }

    void send_arrow(char final_byte) { write_raw(arrow_sequence(final_byte)); }

    void send_delete() { write_raw("\x1b[3~"); }

    void stop() {
        if (stopped_)
            return;
        stopped_ = true;
        bool killed = false;
        // Ask the app to exit through its own quit path (Ctrl+C) instead of
        // killing it: a normal return runs the atexit handlers, which is where
        // gcov flushes. Keep draining the pty while waiting — the app blocks on
        // write once the terminal buffer fills, and a blocked app never reads
        // the quit key. SIGKILL stays as a bounded last resort so the suite can
        // never hang, but a session that needs it is a failure (see main()).
        if (master >= 0 && pid > 0) {
            dismiss_modal();
            write_raw("\x03");
            wait_for_quit();
        }
        // Close the master before reaping: a child still holding the controlling
        // terminal as it exits stays in the kernel's "trying to exit" state on
        // macOS and a blocking waitpid() never returns.
        if (master >= 0) {
            close(master);
            master = -1;
        }
        // Re-check now the master is closed. A child that exited through its own
        // quit path while the master was still open can only become reapable at
        // this point, so counting it as force-killed here would be a false
        // positive — exactly the slow-runner case where the exit lands late.
        for (int i = 0; i < 50; ++i) {
            if (reap_if_exited())
                break;
            usleep(20000);
        }
        if (pid > 0) {
            killed = true;
            record_killed_session(name.empty() ? "<unnamed>" : name);
            kill(pid, SIGKILL);
            for (int i = 0; i < 200; ++i) { // bounded reap: never hang the suite
                int status = 0;
                pid_t reaped = waitpid(pid, &status, WNOHANG);
                if (reaped == pid || reaped < 0)
                    break;
                usleep(10000);
            }
            pid = -1;
        }
        // A killed session is already reported; one that exited without the
        // marker ran part of its teardown and then died, which is the silent
        // failure this check exists to turn into a red run.
        if (!killed && pid < 0 && !exit_ready_seen())
            record_missing_exit_ready(name.empty() ? "<unnamed>" : name);
    }

    bool stopped_ = false;
};

// Sessions that had to be force-killed instead of exiting through the app's own
// quit path. A killed session never runs its atexit handlers, so gcov cannot
// flush the TUI's coverage. Each entry names the scenario.
const std::vector<std::string>& killed_sessions();

// Sessions that left the pty without the app announcing the end of its quit
// path. An exit nobody announced is a failure even when the child is not
// killed.
const std::vector<std::string>& sessions_without_exit_ready();

bool write_file(const std::string& path, const std::string& body);

// A stand-in command that blocks for `seconds`, for "did the UI thread wait on
// this?" measurements: prepend the directory to PATH.
bool write_slow_command(const std::string& dir, const std::string& name, int seconds);
bool write_slow_git(const std::string& dir, int seconds);

// A private workspace under the system temp dir with XDG config/state/cache
// roots and a settings file. Returns empty on failure. Sets AMBER_WORKSPACE and
// the XDG variables, so call it before starting the app.
std::string make_isolated_root();

// The status line carries the workspace directory name; that plus the command
// tree loading is the "the app is ready for input" marker.
bool wait_ready(Tui& tui, const std::string& workspace_name);

// Report a failed step once, with the screen state, and count it.
bool require(bool ok, const std::string& what, const Tui& tui);

std::string text_since(const Tui& tui, std::size_t mark);
int count_of(const std::string& haystack, const std::string& needle);
void assert_no_escape_tail(const std::string& delta, const std::string& tail);

} // namespace agent::test::pty

#endif // AMBER_TESTS_PTY_HARNESS_H

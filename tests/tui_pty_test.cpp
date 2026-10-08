// Regression guard for the modal key path.
//
// A modal reads its own ncurses window, so that window must have keypad()
// enabled: without it ncurses does no escape-sequence translation and an arrow
// key arrives as the raw bytes ESC [ B, whose leading ESC every modal reads as
// "cancel". That is how the session browser used to close on the first Down
// press, leaving the rest of the sequence ("[B") typed into the prompt.
//
// The test acts as a terminal: it drives the real binary in a pty and sends the
// bytes a real terminal would send, which depend on the cursor-key mode ncurses
// put the terminal in (smkx -> SS3 "ESC O B", rmkx -> CSI "ESC [ B"). Asserting
// on the resulting behaviour — not on the keypad flag, which ncurses does not
// expose — is what makes this a guard rather than a restatement of the fix.

#include <fcntl.h>
#include <csignal>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agent/model_probe.h"
#include "agent/session.h"
#include "tests/minitest.h"

namespace {

namespace fs = std::filesystem;

constexpr int kPumpMs = 50;

// Sessions that had to be force-killed instead of exiting through the app's own
// quit path. A killed session never runs its atexit handlers, so gcov cannot
// flush the TUI's coverage (tui/ would report 0%); this must stay empty. Each
// entry names the scenario so a failure points at the session that misbehaved.
std::vector<std::string> killed_sessions;

// Drop terminal control sequences so assertions can look at rendered text.
std::string strip_ansi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        auto c = static_cast<unsigned char>(in[i]);
        if (c != 0x1b) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        ++i;
        if (i >= in.size())
            break;
        if (in[i] == '[') { // CSI: params then a final byte
            ++i;
            while (i < in.size() && (in[i] < 0x40 || in[i] > 0x7e))
                ++i;
            ++i;
        } else if (in[i] == ']') { // OSC: runs to BEL or ST
            ++i;
            auto at_terminator = [&] {
                return in[i] == 0x07 || (in[i] == 0x1b && i + 1 < in.size() && in[i + 1] == '\\');
            };
            while (i < in.size() && !at_terminator())
                ++i;
            i += (i < in.size() && in[i] == 0x1b) ? 2 : 1;
        } else if (in[i] == '(' || in[i] == ')') { // charset designation
            i += 2;
        } else {
            ++i; // ESC = / ESC > and friends
        }
    }
    return out;
}

struct Tui {
    int master = -1;
    pid_t pid = -1;
    std::string raw;
    std::string name; // scenario name, reported if the session is force-killed

    ~Tui() { stop(); }

    bool start(const std::string& binary, const std::string& workspace,
               const std::string& path_prefix = "") {
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
            setenv("TERM", "xterm-256color", 1);
            // Deliver a lone Esc promptly: ncurses otherwise waits ESCDELAY
            // (1 s by default) to decide it is not an escape-sequence prefix,
            // which would stall the teardown below.
            setenv("ESCDELAY", "25", 1);
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
            execl(binary.c_str(), binary.c_str(), static_cast<char*>(nullptr));
            std::fprintf(stderr, "pty child: exec(%s): %s\n", binary.c_str(), std::strerror(errno));
            _exit(127);
        }
        int flags = fcntl(master, F_GETFL, 0);
        fcntl(master, F_SETFL, flags | O_NONBLOCK);
        return true;
    }

    // Read whatever the app has produced. Returns true if any bytes arrived.
    bool pump(int ms) {
        bool got = false;
        int waited = 0;
        while (waited < ms) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(master, &rfds);
            timeval tv{0, static_cast<suseconds_t>(kPumpMs) * 1000};
            int ready = select(master + 1, &rfds, nullptr, nullptr, &tv);
            if (ready <= 0)
                break;
            char buf[8192];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                raw.append(buf, static_cast<size_t>(n));
                got = true;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
            waited += kPumpMs;
        }
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
        // Ask the app to exit through its own quit path (Ctrl+C) instead of
        // killing it: a normal return runs the atexit handlers, which is where
        // gcov flushes. Keep draining the pty while waiting — the app blocks on
        // write once the terminal buffer fills, and a blocked app never reads
        // the quit key. SIGKILL stays as a bounded last resort so the suite can
        // never hang, but a session that needs it is a failure (see main()).
        if (master >= 0 && pid > 0) {
            // Esc first: a test may end inside a modal (e.g. the session
            // browser), which reads its own window and would swallow Ctrl+C.
            // The pty child runs with a short ESCDELAY so a lone Esc lands, but
            // the gap must outlast it — pump() returns as soon as input drains,
            // so hold the pause explicitly.
            write_raw("\x1b");
            for (int i = 0; i < 8; ++i) { // ~0.8 s, draining throughout
                pump(kPumpMs);
                usleep(kPumpMs * 1000);
            }
            write_raw("\x03");
            // Bounded, but generous: a slow CI runner needs room to process the
            // quit key and unwind, and the SIGKILL below is a failure, not a
            // normal path. Draining throughout is what lets the app read the key.
            for (int i = 0; i < 300 && pid > 0; ++i) { // up to ~15 s
                pump(kPumpMs);
                int status = 0;
                pid_t reaped = waitpid(pid, &status, WNOHANG);
                if (reaped == pid || reaped < 0)
                    pid = -1;
            }
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
        for (int i = 0; i < 50 && pid > 0; ++i) {
            int status = 0;
            pid_t reaped = waitpid(pid, &status, WNOHANG);
            if (reaped == pid || reaped < 0)
                pid = -1;
            else
                usleep(20000);
        }
        if (pid > 0) {
            killed_sessions.push_back(name.empty() ? "<unnamed>" : name);
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
    }
};

// Hermetic fixture: a private workspace (sessions + settings) and a private XDG
bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    f << body;
    return static_cast<bool>(f);
}

// Seed two providers' catalogues so /set model has a row per provider and the
// drawer can offer a tab each. Real provider service + real cache format, not a
// stubbed tree: the keys the tabs derive themselves from are exactly these.
bool seed_two_provider_catalogues(const std::string& xdg_home) {
    // xdg_home IS XDG_CONFIG_HOME, and global_config_dir() appends "/amber".
    const std::string dir = xdg_home + "/amber/providers";
    if (!fs::create_directories(dir))
        return false;
    const char* bodies[] = {
        R"({"data":[{"id":"beta-large","context_length":100000},{"id":"beta-small","context_length":8000}]})",
        R"({"data":[{"id":"alpha-shared","context_length":200000}]})"};
    const char* names[] = {"beta", "alpha"};
    for (int i = 0; i < 2; ++i) {
        agent::Config cfg;
        cfg.provider_name = names[i];
        cfg.api_base = "http://127.0.0.1:9/" + std::string(names[i]) + "/v1";
        cfg.flavor = "openai";
        cfg.api_key = "sk-test";
        cfg.model = (i == 0) ? "beta-large" : "alpha-shared";
        const std::string p = dir + "/" + names[i] + ".conf";
        if (!write_file(p, "provider=" + std::string(names[i]) + "\n" + "api_base=" + cfg.api_base +
                               "\n" + "flavor=openai\napi_key=sk-test\n" + "model=" + cfg.model +
                               "\n"))
            return false;
        agent::model_catalog_write(cfg, bodies[i]);
    }
    return true;
}

// The strip renders each tab as "name(count)", so "All(" is a marker nothing else
// on screen produces. Matching a bare provider name would false-positive on the
// seeded session titles ("hello from pty-beta").
bool open_set_model(Tui& tui) {
    tui.send("/set model ");
    return tui.wait_for("All(", 10000);
}

// Type a fragment into the open model drawer and run the row it leaves. The
// dispatch line proves the row existed, was selected, and named its provider --
// and, since neither fragment is a prefix of the id or of the composite key,
// that the search matches beyond the start of a name.
bool pick_by_fragment(Tui& tui, const std::string& fragment, const std::string& expect) {
    tui.send(fragment);
    tui.pump(500);
    tui.send("\r");
    tui.pump(400);
    tui.send("\r"); // the first Enter may only descend; the second is a no-op then
    return tui.wait_for(expect, 10000);
}

// config root, so the test never reads or writes the developer's real state.
struct Fixture {
    std::string workspace;
    std::string workspace_name; // shown in the input prompt: the UI-is-up marker
    std::string binary;
    std::string newer_id = "pty-newer";
    std::string older_id = "pty-older";
};

// Two sessions on one day, with fixed timestamps so list order (newest first) is
// deterministic: row 1 is `newer`, row 2 is `older`.
bool build_fixture(Fixture& fx) {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec) / "amber-pty-XXXXXX";
    if (ec)
        return false;
    std::string tmpl = base.string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data()))
        return false;
    fx.workspace = buf.data();
    fx.workspace_name = fs::path(fx.workspace).filename().string();

    fs::create_directories(fx.workspace + "/.amber");
    fs::create_directories(fx.workspace + "/xdg");
    setenv("AMBER_WORKSPACE", fx.workspace.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (fx.workspace + "/xdg").c_str(), 1);
    if (!write_file(fx.workspace + "/.amber/settings",
                    "api_base=http://127.0.0.1:9/v1\nmodel=pty-test\n"))
        return false;

    agent::SessionStore store;
    struct Seed {
        const std::string* id;
        const char* title;
        long long updated_ms;
    };
    const long long day = 1797000000000LL; // same UTC day for both
    const Seed seeds[] = {{&fx.older_id, "pty-alpha", day},
                          {&fx.newer_id, "pty-beta", day + 600000}};
    for (const auto& seed : seeds) {
        agent::Session s;
        s.id = *seed.id;
        s.title = seed.title;
        s.model = "pty-test";
        s.messages.push_back(agent::Message{"user", "hello from " + s.title, "", "", "", nullptr});
        if (!store.save(s))
            return false;
        // save() stamps updated_ms with now(); pin it so the listing order is fixed.
        agent::json j = s.to_json();
        j["updated_ms"] = seed.updated_ms;
        if (!write_file(store.dir() + "/" + s.id + ".json", j.dump(2)))
            return false;
    }
    store.rebuild_index();
    // Restore a chat window at startup: the first-launch window is a welcome
    // window, which renders art instead of scrollback lines, so command output
    // (append_line) would be invisible there.
    agent::WorkspaceState ws;
    agent::WorkspaceState::WindowEntry we;
    we.session_id = fx.newer_id;
    we.title = "pty-beta";
    ws.windows.push_back(we);
    ws.active = 0;
    if (!store.save_workspace(ws))
        return false;
    // Two providers with catalogues, so the model drawer has a row per provider
    // and can offer a tab each.
    return seed_two_provider_catalogues(fx.workspace + "/xdg");
}

// A stand-in command that blocks for `seconds`. Used to make "did the UI thread
// wait on this?" measurable: prepend the directory to PATH.
bool write_slow_command(const std::string& dir, const std::string& name, int seconds) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return false;
    const std::string path = dir + "/" + name;
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    f << "#!/bin/sh\nsleep " << seconds << "\n";
    f.close();
    fs::permissions(path, fs::perms::owner_all, ec);
    return !ec;
}

// A `git` that blocks for `seconds`: makes "did the UI thread fork git?" a
// measurable question rather than an inspection.
bool write_slow_git(const std::string& dir, int seconds) {
    return write_slow_command(dir, "git", seconds);
}

Fixture& fixture() {
    static Fixture fx;
    static bool ready = build_fixture(fx);
    if (!ready) {
        std::cerr << "FAIL: could not build the pty test fixture\n";
        std::exit(1);
    }
    return fx;
}

// Open the browser and confirm it is on screen before returning.
// The input prompt carries the workspace directory name, so it doubles as a
// "the UI is up" marker — the TUI ticks its clock, so waiting for quiet never
// settles.
bool wait_ready(Tui& tui) {
    // "loaded completions" means the command tree is in place. Typing before it
    // lands can be dispatched against a stale drawer selection, and the prompt
    // marker alone appears earlier than that (the workspace path shows up in
    // startup output too).
    if (!tui.wait_for("loaded completions", 20000))
        return false;
    if (!tui.wait_for("[" + fixture().workspace_name + "]\u2500", 10000))
        return false;
    // Settle: the tree is loaded, but plugin registration and the first feeds
    // still land after the prompt appears, and typing into a half-built tree is
    // dispatched against a stale drawer.
    tui.pump(1000);
    return true;
}

bool open_browser(Tui& tui) {
    if (!wait_ready(tui))
        return false;
    tui.send("/session list\r");
    return tui.wait_for("Sessions", 10000);
}

// Report a failed step once, with the screen state, and stop the test.
bool require(bool ok, const char* what, const Tui& tui) {
    if (ok)
        return true;
    std::cerr << "FAIL: " << what << "\n";
    tui.dump();
    failed++;
    return false;
}

std::string text_since(const Tui& tui, size_t mark) {
    return strip_ansi(tui.raw.substr(mark));
}

void assert_no_escape_tail(const std::string& delta, const std::string& tail) {
    if (delta.find(tail) == std::string::npos)
        return;
    std::cerr << "FAIL: escape-sequence tail '" << tail << "' leaked into the prompt\n";
    failed++;
}

} // namespace

// ── Down moves the selection; it must not cancel the browser ──

TEST(down_arrow_selects_the_next_session) {
    Tui tui;
    tui.name = "down_arrow_selects_the_next_session";
    ASSERT(tui.start(fixture().binary, fixture().workspace));
    if (!require(open_browser(tui), "session browser did not open", tui))
        return;

    size_t mark = tui.raw.size();
    const std::string tail = tui.arrow_tail('B');
    tui.send_arrow('B');
    tui.pump(400); // the app handles a key immediately; no need to wait for quiet
    assert_no_escape_tail(text_since(tui, mark), tail);

    tui.send("\r"); // accept: loads the row Down moved to
    require(tui.wait_for("loaded session " + fixture().older_id, 10000),
            "Down did not move the selection (Enter loaded the wrong session)", tui);
}

// ── Up at the top row is a no-op: the first session stays selected ──

TEST(up_arrow_keeps_the_first_session_selected) {
    Tui tui;
    tui.name = "up_arrow_keeps_the_first_session_selected";
    ASSERT(tui.start(fixture().binary, fixture().workspace));
    if (!require(open_browser(tui), "session browser did not open", tui))
        return;

    size_t mark = tui.raw.size();
    const std::string tail = tui.arrow_tail('A');
    tui.send_arrow('A');
    tui.pump(400);
    assert_no_escape_tail(text_since(tui, mark), tail);

    tui.send("\r");
    require(tui.wait_for("loaded session " + fixture().newer_id, 10000),
            "Up at the top row moved the selection or cancelled the browser", tui);
}

// ── Delete must reach the browser as KEY_DC and raise its confirmation ──

TEST(delete_arrow_opens_the_confirmation_instead_of_cancelling) {
    Tui tui;
    tui.name = "delete_arrow_opens_the_confirmation_instead_of_cancelling";
    ASSERT(tui.start(fixture().binary, fixture().workspace));
    if (!require(open_browser(tui), "session browser did not open", tui))
        return;

    size_t mark = tui.raw.size();
    tui.send_delete();
    if (!require(tui.wait_for("Delete Session", 8000),
                 "Delete did not reach the browser (no confirmation raised)", tui))
        return;
    assert_no_escape_tail(text_since(tui, mark), "[3~");

    tui.send("\r"); // Enter on the default "No" declines; the fixture must
                    // survive for the other tests, and the modal must close.
    tui.pump(400);
}

// ── T2: the UI thread never forks git, and never waits on a command ──

TEST(startup_paints_before_git_returns) {
    // A git that blocks for 5 s. `git_refresh()` runs two of them, so a startup
    // that waits on git cannot paint before ~10 s. The first paint must not wait.
    ASSERT(write_slow_git(fixture().workspace + "/slowbin", 5));
    Tui tui;
    tui.name = "startup_paints_before_git_returns";
    ASSERT(tui.start(fixture().binary, fixture().workspace, fixture().workspace + "/slowbin"));

    auto t0 = std::chrono::steady_clock::now();
    const bool up = tui.wait_for("[" + fixture().workspace_name + "]", 15000);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    if (!require(up, "the UI never painted", tui))
        return;
    require(ms < 3000, "the first paint waited on git (startup forked it)", tui);
}

// ── Bracketed paste into a secret form field ──
//
// Two defects at once, and one assertion covers both:
//   - a secret field cleared ncurses' O_PUBLIC, so the field was drawn empty and
//     a pasted token could not be verified on screen;
//   - a bracketed paste begins with ESC, and form_focus treats a bare ESC as
//     cancel, so the first byte closed the dialog before the rest arrived.
//
// If the pasted text is visible AND the editor is still open, paste decoding
// worked and the field is drawn.

TEST(bracketed_paste_fills_a_secret_field_and_keeps_the_editor_open) {
    Tui tui;
    tui.name = "bracketed_paste_fills_a_secret_field_and_keeps_the_editor_open";
    ASSERT(tui.start(fixture().binary, fixture().workspace));
    if (!wait_ready(tui))
        return;

    // "/provider openrouter" has no key configured, so the switch prompts for it
    // through the shared provider editor -- the form with the secret API Key field.
    tui.send("/provider openrouter\r");
    if (!require(tui.wait_for("Configure: openrouter", 10000), "the provider editor did not open",
                 tui))
        return;

    // Focus starts on "Server URL"; the secret field is the next one (Tab moves).
    tui.send("\t");
    tui.pump(300);
    if (!require(tui.wait_for("API Key", 5000), "no API Key field in the editor", tui))
        return;

    tui.send("\x1b[200~pasted-token-value\x1b[201~");
    tui.pump(600);

    // Still the editor? A paste misread as ESC would have cancelled out of it.
    require(tui.text().find("Configure: openrouter") != std::string::npos,
            "the paste closed the editor -- ESC was read as cancel", tui);
    // On screen? i.e. the field is drawn and the bytes were inserted into it.
    require(tui.wait_for("pasted-token-value", 5000),
            "the pasted text never appeared (paste dropped, or the field is not drawn)", tui);
}

// ── /set model must list every configured provider's cached catalogue ──
//
// The drawer's rows come from the real ProviderService, through endpoints_of()
// -> catalogs_from() -> aggregate_provider_models(). Each link had its own unit
// test, but none exercised the chain as the APP builds it -- open_set_model()
// existed unused -- so nothing noticed that endpoints_of() built its
// ProviderEndpoints without the flag catalogs_from() gates the cache read on:
// every catalogue was skipped and /set model opened an empty drawer. With no
// rows there was also no tab strip, which needs two rows even to render.
//
// The search half is proven by dispatch, not by watching a repaint: "large" is
// a suffix of the id and "lph" sits mid-id, so a prefix-only match finds
// neither, and Enter on the filtered row must land as the model set -- naming
// the provider it switched to. Tab COUNTS are deliberately not asserted from
// the byte stream: the strip is diffed into the terminal, so a changed count
// arrives as a bare digit and "All(3)" is never contiguous. The strip's layout
// and counts are unit tested in drawer_tabs_test; what this test adds is the
// wiring beneath.
TEST(set_model_lists_every_configured_providers_cached_models) {
    Tui tui;
    tui.name = "set_model_lists_every_configured_providers_cached_models";
    ASSERT(tui.start(fixture().binary, fixture().workspace));
    if (!require(wait_ready(tui), "the UI never came up", tui))
        return;

    if (!require(open_set_model(tui), "the drawer has no rows, so it has no tab strip", tui))
        return;

    require(tui.wait_for("beta::beta-large", 5000), "beta's catalogue is missing from the drawer",
            tui);
    require(tui.wait_for("beta::beta-small", 5000), "beta's second model is missing", tui);
    require(tui.wait_for("alpha::alpha-shared", 5000), "alpha's catalogue is missing", tui);

    // One dispatch per provider, both by a fragment that matches past the start.
    require(pick_by_fragment(tui, "large", "model set to beta-large"),
            "a suffix match was not selectable (search matches prefixes only?)", tui);
    // The pick switches provider (the line says "remembered for beta"), and the
    // switch must land in the WINDOW's agent: the bar reads its provider_name and
    // the next request reads its endpoint. Leaving the agent behind showed the
    // old provider beside the new model -- and sent that model to the old
    // endpoint, which is a kilo model routed to openrouter.
    require(tui.wait_for("beta|beta-large", 10000),
            "the bar does not show the provider that owns the picked model", tui);
    tui.send("/set model ");
    tui.pump(600);
    require(pick_by_fragment(tui, "lph", "model set to alpha-shared"),
            "the other provider's catalogue is not searchable or selectable", tui);
}

// ── the provider tabs narrow the list (and therefore what Enter runs) ──
//
// Switching a tab used to move only the strip's marker: the rows painted, the
// rows Enter ran and the tab counts were three different sets. A fresh instance,
// because the assertion below is "a string that has never been on this screen",
// which only stays sound if nothing earlier in the same app could print it.
//
// No provider is active yet, so the rows follow the sorted provider order
// (alpha's block first) and two Rights reach beta's tab. Down then selects beta
// SMALL -- a model no other step sets. On the unfiltered list Down lands on
// beta-large instead, so the line can only appear if the tab really narrowed
// the rows.
TEST(set_model_provider_tabs_narrow_the_rows) {
    Tui tui;
    tui.name = "set_model_provider_tabs_narrow_the_rows";
    ASSERT(tui.start(fixture().binary, fixture().workspace));
    if (!require(wait_ready(tui), "the UI never came up", tui))
        return;
    if (!require(open_set_model(tui), "the drawer has no rows, so it has no tab strip", tui))
        return;

    tui.send_arrow('C'); // All -> alpha
    tui.pump(300);
    tui.send_arrow('C'); // alpha -> beta
    tui.pump(300);
    tui.send_arrow('B'); // inside beta's tab: beta-large -> beta-small
    tui.pump(300);
    tui.send("\r");
    tui.pump(400);
    tui.send("\r"); // the first Enter may only descend; the second is a no-op then
    require(tui.wait_for("model set to beta-small", 10000),
            "a tab switch did not narrow the rows: Down ran a row from another provider", tui);
}

TEST(system_commands_do_not_freeze_the_ui) {
    // `/system ps` dispatches reliably (a leaf, no argument). A `ps` shim that
    // blocks for 4 s turns it into a long job, so "did the UI wait on it?" is
    // measurable: a UI that polls the job cannot acknowledge the start, and
    // cannot echo a keystroke, before the command finishes.
    ASSERT(write_slow_command(fixture().workspace + "/slowbin", "ps", 4));
    Tui tui;
    tui.name = "system_commands_do_not_freeze_the_ui";
    ASSERT(tui.start(fixture().binary, fixture().workspace, fixture().workspace + "/slowbin"));
    if (!require(wait_ready(tui), "the UI never came up", tui))
        return;
    tui.pump(4000); // settle to idle: the ack window below measures the app's
                    // latency, not its startup

    // Two Enters, deliberately: the drawer's first Enter can select/descend and
    // the second dispatches. On an already-dispatched (now empty) prompt the
    // extra Enter is a no-op, so this is robust to either semantics.
    tui.send("/system ps");
    tui.pump(400);
    tui.send("\r");
    tui.pump(400);
    tui.send("\r");
    if (!require(tui.wait_for("ps: started", 3000),
                 "/system ps did not acknowledge the start within 1.5 s", tui))
        return;

    // The UI stays live while the job runs: a keystroke still reaches the prompt.
    size_t mark = tui.raw.size();
    tui.send("z");
    bool echoed = false;
    for (int waited = 0; waited < 1500 && !echoed; waited += 50) {
        tui.pump(50);
        echoed = text_since(tui, mark).find('z') != std::string::npos;
    }
    if (!require(echoed, "the UI stopped accepting input while a command ran", tui))
        return;

    // ...and the result is reported when the job finishes, on the drain.
    require(tui.wait_for("ps: exit 0", 20000), "the job result never arrived", tui);
}

int main(int argc, char** argv) {
    // Absolute: the child chdir()s into the workspace before exec.
    std::error_code ec;
    fixture().binary = fs::absolute(argc > 1 ? argv[1] : "./amber", ec).string();
    if (ec || !fs::exists(fixture().binary)) {
        std::cerr << "FAIL: amber binary not found at " << fixture().binary << "\n";
        return 1;
    }

    down_arrow_selects_the_next_session();
    up_arrow_keeps_the_first_session_selected();
    delete_arrow_opens_the_confirmation_instead_of_cancelling();
    startup_paints_before_git_returns();
    bracketed_paste_fills_a_secret_field_and_keeps_the_editor_open();
    set_model_lists_every_configured_providers_cached_models();
    set_model_provider_tabs_narrow_the_rows();
    system_commands_do_not_freeze_the_ui();

    // The TUI must terminate through its own quit path: a force-killed session
    // never flushes its gcov counters, which is what pinned tui/ coverage at 0%.
    if (!killed_sessions.empty()) {
        for (const std::string& session : killed_sessions)
            std::cerr << "FAIL: session '" << session
                      << "' was force-killed instead of exiting through the quit path\n";
        failed += static_cast<int>(killed_sessions.size());
    }

    if (failed)
        std::cout << "FAILED (" << failed << " failures)\n";
    else
        std::cout << "ALL PASSED\n";
    return failed ? 1 : 0;
}

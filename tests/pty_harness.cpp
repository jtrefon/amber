#include "tests/pty_harness.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agent/exit_ready.h"
#include "agent/workspace.h"
#include "tests/minitest.h"

namespace agent::test::pty {

namespace {

std::vector<std::string>& killed_sessions_mut() {
    static std::vector<std::string> sessions;
    return sessions;
}

std::vector<std::string>& sessions_without_exit_ready_mut() {
    static std::vector<std::string> sessions;
    return sessions;
}

} // namespace

void record_killed_session(const std::string& name) {
    killed_sessions_mut().push_back(name);
}

void record_missing_exit_ready(const std::string& name) {
    sessions_without_exit_ready_mut().push_back(name);
}

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

const std::vector<std::string>& killed_sessions() {
    return killed_sessions_mut();
}
const std::vector<std::string>& sessions_without_exit_ready() {
    return sessions_without_exit_ready_mut();
}

bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    f << body;
    return static_cast<bool>(f);
}

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

bool write_slow_git(const std::string& dir, int seconds) {
    return write_slow_command(dir, "git", seconds);
}

std::string make_isolated_root() {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec) / "amber-pty-XXXXXX";
    if (ec)
        return {};
    std::string tmpl = base.string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data()))
        return {};
    const std::string workspace = buf.data();
    fs::create_directories(workspace + "/xdg", ec);
    setenv("AMBER_WORKSPACE", workspace.c_str(), 1);
    // project_root() caches its first answer for the process lifetime; a
    // second isolated root in the same test binary has to move it.
    agent::Workspace::set_root(workspace);
    setenv("XDG_CONFIG_HOME", (workspace + "/xdg").c_str(), 1);
    setenv("XDG_STATE_HOME", (workspace + "/state").c_str(), 1);
    setenv("XDG_CACHE_HOME", (workspace + "/cache").c_str(), 1);
    fs::create_directories(fs::path(agent::Workspace::settings_path()).parent_path(), ec);
    if (!write_file(agent::Workspace::settings_path(),
                    "api_base=http://127.0.0.1:9/v1\nmodel=pty-test\n"))
        return {};
    return workspace;
}

bool wait_ready(Tui& tui, const std::string& workspace_name) {
    // "loaded completions" means the command tree is in place. Typing before it
    // lands can be dispatched against a stale drawer selection, and the prompt
    // marker alone appears earlier than that (the workspace path shows up in
    // startup output too).
    if (!tui.wait_for("loaded completions", 20000))
        return false;
    if (!tui.wait_for("[" + workspace_name + "]\u2500", 10000))
        return false;
    // Settle: the tree is loaded, but plugin registration and the first feeds
    // still land after the prompt appears, and typing into a half-built tree is
    // dispatched against a stale drawer.
    tui.pump(1000);
    return true;
}

bool require(bool ok, const std::string& what, const Tui& tui) {
    if (ok)
        return true;
    std::cerr << "FAIL: " << what << "\n";
    tui.dump();
    failed++;
    return false;
}

std::string text_since(const Tui& tui, std::size_t mark) {
    return strip_ansi(tui.raw.substr(mark));
}

int count_of(const std::string& haystack, const std::string& needle) {
    int count = 0;
    for (std::size_t pos = 0; (pos = haystack.find(needle, pos)) != std::string::npos;
         pos += needle.size())
        ++count;
    return count;
}

void assert_no_escape_tail(const std::string& delta, const std::string& tail) {
    if (delta.find(tail) == std::string::npos)
        return;
    std::cerr << "FAIL: escape-sequence tail '" << tail << "' leaked into the prompt\n";
    failed++;
}

} // namespace agent::test::pty

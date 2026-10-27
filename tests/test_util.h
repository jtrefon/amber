
#ifndef AGENT_TEST_UTIL_H
#define AGENT_TEST_UTIL_H

// Minimal, dependency-free unit-test harness for libagent. No external test
// framework is required so the project stays friendly to a from-source,
// autotools-free build on minimal Linux servers.
//
// Usage:
//   TEST(name) { ... ASSERT(cond); ASSERT_EQ(a, b); ... }
//   int main(int argc, char** argv) { return agent::test::run_all(argc, argv); }
//
//   ./run_tests --list          list every test, one per line
//   ./run_tests <substring>     run the tests whose name contains it
//   RUN_TESTS_ONLY=<substring>  same filter, for scripts that pass no args
//
// A test that hangs is a loud failure, not a stuck run: AMBER_TEST_TIMEOUT_MS
// (default 120000, 0 disables) arms a per-test alarm that names the test and
// exits non-zero.

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace agent {
namespace test {

struct Case {
    std::string name;
    void (*fn)() = nullptr;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int failures = 0;
inline int passed = 0;
inline const char* g_only = std::getenv("RUN_TESTS_ONLY");
inline const char* g_current = nullptr;

struct Registrar {
    Registrar(const std::string& name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Async-signal-safe write: one syscall, result deliberately ignored (the
// watchdog cannot do anything about a failed write before it exits).
inline void safewrite(const char* what, size_t n) noexcept {
    const ssize_t ignored = ::write(STDOUT_FILENO, what, n);
    (void)ignored;
}

// Async-signal-safe: one write and _exit, so it works while the test is blocked
// inside a read() on a pipe nobody will ever write to.
inline void alarm_handler(int) {
    const char* name = g_current ? g_current : "?";
    safewrite("[ TIMEOUT ] ", 12);
    safewrite(name, std::strlen(name));
    safewrite("\n", 1);
    ::_exit(1);
}

inline long watchdog_seconds() {
    const char* v = std::getenv("AMBER_TEST_TIMEOUT_MS");
    const long ms = v ? std::strtol(v, nullptr, 10) : 120000L;
    if (ms <= 0)
        return 0;
    const long s = (ms + 999) / 1000;
    return s < 1 ? 1 : s;
}

inline bool matches(const Case& c, const std::string& filter) {
    if (!filter.empty() && c.name.find(filter) == std::string::npos)
        return false;
    return !(g_only && c.name.find(g_only) == std::string::npos);
}

inline int run_all(int argc = 0, char** argv = nullptr) {
    std::string filter;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        // argc and argv are independent parameters: a caller may pass a count
        // without the vector, and the default argv is null.
        const std::string arg = (argv != nullptr && argv[i] != nullptr) ? argv[i] : "";
        if (arg == "--list")
            list_only = true;
        else if (!arg.empty())
            filter = arg;
    }
    if (list_only) {
        for (const auto& c : registry())
            if (matches(c, filter))
                std::cout << c.name << "\n";
        return 0;
    }

    const long watchdog = watchdog_seconds();
    if (watchdog > 0)
        std::signal(SIGALRM, alarm_handler);

    for (const auto& c : registry()) {
        if (!matches(c, filter))
            continue;
        g_current = c.name.c_str();
        if (watchdog > 0)
            alarm(static_cast<unsigned>(watchdog));
        try {
            c.fn();
            ++passed;
            std::cout << "[ PASS ] " << c.name << "\n";
        } catch (const std::string& msg) {
            ++failures;
            std::cout << "[ FAIL ] " << c.name << ": " << msg << "\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cout << "[ FAIL ] " << c.name << ": exception: " << e.what() << "\n";
        } catch (...) {
            ++failures;
            std::cout << "[ FAIL ] " << c.name << ": unknown exception\n";
        }
        if (watchdog > 0)
            alarm(0);
        g_current = nullptr;
    }
    std::cout << "\n" << passed << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}

[[noreturn]] inline void fail(const std::string& msg) {
    throw msg;
}

// Redirect the XDG roots to a scratch tree for a test body: the model catalog
// cache lives under the cache root, per-project state (sessions, artifacts)
// under the state root, and tests must never read or write the developer's real
// ~/.config, ~/.cache or ~/.local/state.
struct XdgGuard {
    explicit XdgGuard(const std::string& name)
        : dir("/tmp/amber_xdg_" + name), state(dir + "/state"), cache(dir + "/cache") {
        std::filesystem::remove_all(dir);
        capture("XDG_CONFIG_HOME", saved_config, had_config);
        capture("XDG_STATE_HOME", saved_state, had_state);
        capture("XDG_CACHE_HOME", saved_cache, had_cache);
        setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
        setenv("XDG_STATE_HOME", state.c_str(), 1);
        setenv("XDG_CACHE_HOME", cache.c_str(), 1);
    }
    ~XdgGuard() {
        restore("XDG_CONFIG_HOME", saved_config, had_config);
        restore("XDG_STATE_HOME", saved_state, had_state);
        restore("XDG_CACHE_HOME", saved_cache, had_cache);
        std::filesystem::remove_all(dir);
    }
    XdgGuard(const XdgGuard&) = delete;
    XdgGuard& operator=(const XdgGuard&) = delete;

    std::string dir;
    std::string state;
    std::string cache;
    std::string saved_config;
    std::string saved_state;
    std::string saved_cache;
    bool had_config = false;
    bool had_state = false;
    bool had_cache = false;

private:
    static void capture(const char* name, std::string& out, bool& had) {
        const char* v = std::getenv(name);
        had = v != nullptr;
        if (v)
            out = v;
    }
    static void restore(const char* name, const std::string& saved, bool had) {
        if (had)
            setenv(name, saved.c_str(), 1);
        else
            unsetenv(name);
    }
};

} // namespace test
} // namespace agent

#define TEST(name)                                                                                 \
    static void test_##name();                                                                     \
    static ::agent::test::Registrar reg_##name(#name, test_##name);                                \
    static void test_##name()

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::ostringstream _os;                                                                \
            _os << "assert failed: " #cond << " (" << __FILE__ << ":" << __LINE__ << ")";          \
            ::agent::test::fail(_os.str());                                                        \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        auto _va = (a);                                                                            \
        auto _vb = (b);                                                                            \
        if (!(_va == _vb)) {                                                                       \
            std::ostringstream _os;                                                                \
            _os << "assert_eq failed: " #a " == " #b << " (" << __FILE__ << ":" << __LINE__        \
                << ") -> " << _va << " != " << _vb;                                                \
            ::agent::test::fail(_os.str());                                                        \
        }                                                                                          \
    } while (0)

#define ASSERT_TRUE(cond) ASSERT(cond)
#define ASSERT_FALSE(cond) ASSERT(!(cond))

#endif // AGENT_TEST_UTIL_H

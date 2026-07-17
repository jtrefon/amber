// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_TEST_UTIL_H
#define AGENT_TEST_UTIL_H

// Minimal, dependency-free unit-test harness for libagent. No external test
// framework is required so the project stays friendly to a from-source,
// autotools-free build on minimal Linux servers.
//
// Usage:
//   TEST(name) { ... ASSERT(cond); ASSERT_EQ(a, b); ... }
//   int main() { return agent::test::run_all(); }

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace agent {
namespace test {

struct Case {
    std::string name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int failures = 0;
inline int passed = 0;

struct Registrar {
    Registrar(const std::string& name, void (*fn)()) {
        registry().push_back({name, fn});
    }
};

inline int run_all() {
    for (const auto& c : registry()) {
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
    }
    std::cout << "\n" << passed << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}

[[noreturn]] inline void fail(const std::string& msg) { throw msg; }

} // namespace test
} // namespace agent

#define TEST(name)                                                          \
    static void test_##name();                                              \
    static ::agent::test::Registrar reg_##name(#name, test_##name);        \
    static void test_##name()

#define ASSERT(cond)                                                        \
    do { if (!(cond)) {                                                     \
        std::ostringstream _os; _os << "assert failed: " #cond              \
            << " (" << __FILE__ << ":" << __LINE__ << ")";                 \
        ::agent::test::fail(_os.str());                                     \
    } } while (0)

#define ASSERT_EQ(a, b)                                                     \
    do { auto _va = (a); auto _vb = (b);                                    \
        if (!(_va == _vb)) {                                                \
            std::ostringstream _os; _os << "assert_eq failed: " #a " == " #b \
                << " (" << __FILE__ << ":" << __LINE__ << ") -> "           \
                << _va << " != " << _vb;                                    \
            ::agent::test::fail(_os.str());                                 \
        } } while (0)

#define ASSERT_TRUE(cond) ASSERT(cond)
#define ASSERT_FALSE(cond) ASSERT(!(cond))

#endif // AGENT_TEST_UTIL_H

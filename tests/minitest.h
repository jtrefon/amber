#ifndef AMBER_TESTS_MINITEST_H
#define AMBER_TESTS_MINITEST_H

// Minimal assert machinery shared by the standalone test binaries
// (command_line_test, session_browser_test, e2e_test, completions_test,
// plugin_test, ws_test, url_test). run_tests uses the Registrar harness in
// tests/test_util.h instead.

#include <iostream>

inline int failed = 0;

#define TEST(name) void name()

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL: " << #cond << "\n";                                                \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            std::cerr << "FAIL: " << #a << " == " << #b << "  got: " << (a)                        \
                      << " expected: " << (b) << "\n";                                             \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

#endif // AMBER_TESTS_MINITEST_H

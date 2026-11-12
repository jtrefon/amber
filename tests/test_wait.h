#ifndef AMBER_TESTS_TEST_WAIT_H
#define AMBER_TESTS_TEST_WAIT_H

// Bounded waits for tests that observe another thread or process.
//
// A test that needs an event to happen polls a condition until a deadline; it
// does not sleep for a guessed duration. The default budget is 20 s, which is
// an order of magnitude below the 60 s call deadline these tests guard
// against, so a loaded runner has room while a synchronous implementation
// still fails by a wide margin.
//
// This header deliberately contains no test macros: it is included by both
// tests/test_util.h (the registrar harness) and the standalone minitest
// binaries, which define their own TEST/ASSERT.

#include <chrono>
#include <thread>

namespace agent {
namespace test {

inline constexpr auto kWaitBudget = std::chrono::milliseconds(20000);

template <typename Fn> bool wait_until(Fn fn, std::chrono::milliseconds budget = kWaitBudget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fn();
}

inline long long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 since)
        .count();
}

} // namespace test
} // namespace agent

#endif // AMBER_TESTS_TEST_WAIT_H

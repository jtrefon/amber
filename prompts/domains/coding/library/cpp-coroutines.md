trigger: when async code needs a shape that callbacks and threads both make ugly
tags: #cpp #async
weight: 40
verified: 2026-10
reference: https://en.cppreference.com/w/cpp/language/coroutines

A coroutine is a function containing `co_await`, `co_yield` or `co_return`. The
compiler turns it into a state machine with a frame on the heap; what it needs
from you is the promise type and the awaiters.

- **You supply the policy, the compiler supplies the mechanics.** A `task<T>`
  decides how it is scheduled, cancelled and destroyed by defining
  `promise_type`, `initial_suspend`, `final_suspend` and the awaiter's three
  methods (`await_ready`, `await_suspend`, `await_resume`).
- **The frame outlives the call.** A suspended coroutine holds its locals until
  it is resumed or destroyed; a coroutine nobody owns after suspension is a leak,
  and destroying a suspended frame ends the values it captured.
- **`suspend_never` for eager, `suspend_always` for lazy** is the design decision
  people get wrong first: it decides whether work starts at the call or at the
  first resume.
- **Cancellation is yours to build**: the language gives you no cancel token, so
  a task type decides how a cancelled coroutine is resumed (or destroyed) — and
  the answer belongs in the task type, not at every call site.
- **C++23 ships `std::generator`** for the common generator case; until the
  toolchain has it, a twenty-line promise type does the job.
- **Measure.** A frame is an allocation and every resume is an indirect call;
  coroutines buy readability, not speed.

## Run it

```run
cat > co_demo.cpp <<'SRC'
#include <coroutine>
#include <cstdio>

struct Ready {
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

struct Task {
    struct promise_type {
        Task get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };
};

Task hello() {
    co_await Ready{};
    std::puts("resumed after await");
}

int main() { hello(); }
SRC
g++ -std=c++20 co_demo.cpp -o co_demo
./co_demo | grep -q "resumed after await"
```

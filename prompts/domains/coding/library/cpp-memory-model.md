trigger: when two threads agree in a debugger and disagree in production
tags: #cpp #concurrency #memory
weight: 55
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://en.cppreference.com/w/cpp/atomic/memory_order

A data race is not "two threads touching a variable" — it is two accesses, at
least one a write, not ordered by happens-before. Reasoning about the memory
model beats reasoning about the debugger:

- **`std::atomic` with the default `seq_cst` is correct and slow.** `relaxed` is
  only for a counter nobody reads to make a decision; `acquire`/`release` pairs
  are what a lock-free handoff actually needs, and they are the pair to reach for
  before reaching for `seq_cst` everywhere.
- **A mutex establishes happens-before** for everything written before the unlock
  — the classic mistake is publishing a pointer under a lock and then reading
  the object without it.
- **A race is UB, not a flaky test**: the optimiser may hoist the read out of the
  loop entirely, so "it worked in the debug build" proves nothing.
- **Two locks taken in different orders deadlock**; one lock held across a
  callback is a deadlock that only fires when someone else takes the lock in
  that callback.
- **A thread that outlives its starter** needs a join on every path, including
  the exception path: a `std::thread` destroyed while joinable terminates the
  process.

Prove the race, do not stare at it: ThreadSanitizer names both accesses with
their stacks, and it is the only tool that sees this class at all.

## Run it

```run
cat > racy.cpp <<'SRC'
#include <thread>
int counter = 0;
int main() {
    std::thread a([]{ for (int i = 0; i < 1000000; ++i) ++counter; });
    std::thread b([]{ for (int i = 0; i < 1000000; ++i) ++counter; });
    a.join();
    b.join();
    return counter == 2000000 ? 0 : 1;
}
SRC
g++ -std=c++17 -fsanitize=thread -g racy.cpp -o racy
grep -q "ThreadSanitizer: data race" <(./racy 2>&1 || true)
```

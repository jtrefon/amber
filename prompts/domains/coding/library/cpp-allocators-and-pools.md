trigger: when allocation shows up in a profile, or a hot path allocates per call
tags: #cpp #memory #perf
weight: 50
verified: 2026-10
reference: https://en.cppreference.com/w/cpp/memory/polymorphic_allocator

Allocation is rarely slow; the *pattern* is — thousands of small, scattered
lifetimes that share an owner.

- **Say the lifetime first.** If many objects live and die together, a monotonic
  (arena) resource allocation is one pointer bump and one free for all of them.
  `std::pmr::monotonic_buffer_resource` is that, without writing an allocator.
- **`std::pmr` is the seam**: containers and strings take a `memory_resource*`,
  so the policy is chosen where the container is created and the type stays the
  same.
- **A pool suits fixed-size objects**; a free list is a pool; a pool that never
  returns memory to the OS is a decision, not a bug — say so.
- **`reserve`, `resize`, `emplace_back`** and passing by reference remove most
  allocations before any allocator work is needed.
- **Do not cache a `memory_resource` beyond its lifetime** — a monotonic resource
  freed at scope exit invalidates everything allocated from it, and that is the
  intended semantics.
- **Measure with heaptrack or massif** before and after; allocation count and
  peak footprint are the numbers that move.

## Run it

```run
cat > pmr_demo.cpp <<'SRC'
#include <cstdio>
#include <memory_resource>
#include <vector>

int main() {
    char buffer[4096];
    std::pmr::monotonic_buffer_resource arena(buffer, sizeof buffer);
    std::pmr::vector<int> numbers(&arena);
    for (int i = 0; i < 500; ++i)
        numbers.push_back(i);
    std::printf("size=%zu first=%d last=%d\n", numbers.size(), numbers.front(), numbers.back());
}
SRC
g++ -std=c++17 pmr_demo.cpp -o pmr_demo
./pmr_demo | grep -q "size=500 first=0 last=499"
```

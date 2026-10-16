trigger: when something is slow and the obvious fix is a guess
tags: #cpp #perf #debugging
weight: 50
verified: 2026-10
reference: https://perf.wiki.kernel.org/index.php/Main_Page

Measure, then change one thing. The most expensive habit in C++ is optimising
the code you can read instead of the code the profiler names.

- **`perf record` / `perf report`** for where the time goes, with `-g` for call
  graphs. `perf stat` answers a different question: whether the problem is
  instructions, cache misses or branch mispredictions.
- **A release build with debug symbols** (`-O2 -g`) — profiling `-O0` measures
  code nobody runs.
- **`-pg` / gprof is obsolete**; callgrind (valgrind) gives exact call counts at
  ~50x slowdown, which is right for a small reproducible workload and wrong for
  anything real-time.
- **heaptrack / massif** for allocation volume and peak footprint — often the real
  story behind "slow", especially at startup.
- **Benchmark the change** (`google/benchmark`, or a loop with a clock around
  the smallest callable unit) and keep the number in the commit message. A
  "faster" change with no number is a guess with extra steps.

## Run it

```run
# manual: needs the target project's own workload; the shape is perf record -g ./binary
perf --version
```

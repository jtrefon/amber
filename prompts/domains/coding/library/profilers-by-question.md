trigger: when something is slow or grows and "run a profiler" is the plan without a question yet
tags: #perf #tooling
weight: 50
verified: 2026-10
reference: https://www.brendangregg.com/FlameGraphs/cpuflamegraphs.html
sources: Enberg, Latency (2025), Ch. 2; Rice, Learning eBPF (2023), Ch. 1; Gavrilin, Software Architecture with C++ 2ed (2025), Ch. 13; web pass 2026-10: Gregg (off-CPU flame graphs), async-profiler modes, py-spy, dotnet diagnostics, Go pprof, memray

Each profiler answers one question well and another badly. Name the question
before picking the tool; the answer usually removes a rewrite rather than
causing one.

- **Where does CPU time go?** `perf record -g` then flame graphs; `go tool
  pprof` over `net/http/pprof`; py-spy record or top (sampling, no restart,
  low overhead); `node --prof` plus `--prof-process`, or 0x / Clinic Flame;
  dotnet-trace for EventPipe CPU samples rendered in speedscope;
  async-profiler or JFR on the JVM. Profile a `-O2 -g` build; a `-O0` flame
  graph measures code nobody ships.
- **Why is it not on the CPU?** Off-CPU flame graphs (perf sched plus eBPF
  wakeups), bpftrace's offcputime/offwaketime scripts, `strace -T` or
  `perf trace` for syscall waits; async-profiler's wall-clock mode; the Go
  block profile and `go tool trace`; Node's Clinic Bubbleprof.
- **Why does memory grow?** Valgrind Massif for the heap over time, heaptrack
  for allocation counts with backtraces, memray for Python including native
  frames; dotnet-gcdump and dotnet-counters for the managed heap; the Go heap
  profile; JFR old-object sampling; a V8 heap snapshot.
- **Is allocation churn the cost?** heaptrack's allocation total, an
  allocator's own heap profile (jemalloc or tcmalloc), memray's peak, GC
  events in dotnet-trace, async-profiler's alloc mode. Churn usually surfaces
  as CPU, so prove it before rewriting the hot path.
- **What is the I/O waiting on?** `iostat -x` for devices; biolatency or
  biosnoop (bpftrace) for per-request latency; `perf trace -T`; `strace -T`.
  Separate disk from network from scheduler delay before blaming either.
- **Who holds the lock?** `perf lock contention` (kernel 5.12+), `perf sched`,
  bpftrace's offwaketime; the Go mutex and block profiles; async-profiler's
  lock mode or jstack thread dumps; contention events in dotnet-trace.
- **What does the latency distribution look like?** A mean hides the story.
  Collect a histogram (HdrHistogram) or an eCDF at a fixed request rate, or
  coordinated omission makes the tool's own waiting vanish from the p99 —
  Enberg's Ch. 2 is the worked argument. bpftrace histograms kernel-side
  latencies; OpenTelemetry spans give the per-request breakdown.

Two constraints on all of them: the measurement perturbs the measured (keep
the load generator off the machine under test), and the distribution matters
more than the average that gets quoted.

## Run it

```run
# manual: profilers are not installed in CI and profiling needs the target's own
# workload. The shapes are:
#   perf record -g ./app && perf report
#   py-spy record -o profile.svg -- ./app
#   go tool pprof -http=:8080 http://localhost:6060/debug/pprof/profile
perf --version
```

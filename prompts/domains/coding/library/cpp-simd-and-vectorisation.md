trigger: when a numeric loop is the hot path and the compiler is or is not using SIMD
tags: #cpp #perf
weight: 45
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://gcc.gnu.org/onlinedocs/gcc/Vector-Extensions.html

The compiler vectorises more than people expect and less than they hope; ask it
what it did instead of guessing.

- **Read the loop for dependencies**: a loop the compiler cannot prove independent
  (aliasing, a carried dependency, an early exit) will not vectorise, however
  simple it looks.
- **`-fopt-info-vec` (GCC) / `-Rpass=loop-vectorize` (clang)** report what was
  vectorised and what was not, with the line. That report is the first thing to
  read on a hot loop.
- **Help it**: `restrict`/`__restrict` for pointer arguments promise no aliasing,
  `#pragma omp simd` asserts independence, and iterating over a `std::vector` by
  index (not a range over pointers) keeps alignment information.
- **Alignment and the tail**: unaligned loads are supported but slower; a loop
  count not divisible by the vector width leaves a scalar tail the compiler
  handles.
- **Only then reach for intrinsics.** Hand-written SIMD is a portability and
  maintenance cost; `std::experimental::simd` / `xsimd` are the middle ground.
- **Measure both builds**: `-O2` versus `-O3 -march=native` on the same input —
  if the vectorised version is not faster, the loop was memory-bound, and the fix
  is locality, not width.

## Run it

```run
cat > vec_demo.cpp <<'SRC'
#include <cstdio>
#include <vector>

void scale(double* data, std::size_t count, double factor) {
    for (std::size_t i = 0; i < count; ++i)
        data[i] *= factor;
}

int main() {
    std::vector<double> data(1024, 2.0);
    scale(data.data(), data.size(), 3.0);
    std::printf("%.0f\n", data[512]);
}
SRC
g++ -std=c++17 -O3 -fopt-info-vec vec_demo.cpp -o vec_demo 2> vec_report.txt
grep -q "loop vectorized" vec_report.txt
./vec_demo | grep -q 6
```

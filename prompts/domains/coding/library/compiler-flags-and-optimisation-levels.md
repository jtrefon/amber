trigger: when the release build is slower than debug, or "just add -O3" is the whole plan
tags: #build #perf #tooling
weight: 45
verified: 2026-10
reference: https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html
sources: Gavrilin, Software Architecture with C++ 2ed (2025), Ch. 13; Mishra, Executable Files for Linux (2026), Ch. 2 and 5; GCC Optimize Options; SUSE "GCC 15 optimization" (2025); web pass 2026-10

A `-O` level is a budget of compiler effort, not a speed dial, and it only
ever buys back what the code lets it buy.

- **-O0** builds for the debugger: variables have homes and source maps
  line-for-line. **-O1/-O2** is the usual release point, -O2 enabling
  everything cheap enough to keep compile time sane. **-O3** adds aggressive
  inlining and vectorisation and on real code can lose to -O2 through code
  size and cache pressure. **-Os** trades speed for bytes. Measure a level
  change like any other change.
- **-g is independent of -O.** Ship `-O2 -g`, then strip or split the debug
  sections so a crash is symbolizable without shipping symbols in the binary.
  A release built without -g turns every core dump into guesswork.
- **LTO** (`-flto`, with thin variants on some toolchains) lets the optimizer
  see across translation units for cross-module inlining and
  devirtualisation. It costs link time and memory, so enable it for release
  builds only (CMake: INTERPROCEDURAL_OPTIMIZATION); it does not excuse
  cache-hostile data structures.
- **PGO** (`-fprofile-generate` → run a representative workload →
  `-fprofile-use`) bets on measured branch and call frequencies. The profile
  becomes part of the build: keep it in CI artifacts, regenerate it when the
  workload moves, and never train on a benchmark that does not resemble
  production — a wrong profile optimizes the wrong program.
- **-march/-mtune choose the audience.** `-march=native` binds the binary to
  the build machine and destroys portability and reproducibility;
  `-mtune=generic` keeps a broad target while still scheduling well. Pick an
  explicit baseline (x86-64-v2/v3) and use runtime dispatch for the one path
  that needs AVX.
- **Float switches are a contract.** `-ffast-math` (and `-Ofast`, which is
  -O3 plus it; MSVC `/fp:fast`) drops NaN/Inf semantics, signed zeros and
  reassociation; it belongs on one file with tests pinning the acceptable
  error, never in a global release flag. `-fno-math-errno` is a smaller
  promise and often enough.
- **Reproducibility is a flag set, not a hope.** The same compiler version and
  flags everywhere (CMake presets, Cargo profiles), no `__DATE__`/`__TIME__`
  (`-Wdate-time`), `-ffile-prefix-map` or `--remap-path-prefix` to strip build
  paths, `SOURCE_DATE_EPOCH` for the rest. A release that differs between CI
  and a laptop is a release you cannot bisect.

## Run it

```run
# The -O level reaches the preprocessor; both binaries agree; -g survives -O2.
printf 'int f(int n){int s=0;for(int i=0;i<n;i++)s+=i;return s;}\nint main(void){return f(4)==6?0:1;}\n' > prog.c
test "$(gcc -O0 -dM -E prog.c | grep -c __OPTIMIZE__)" = 0
test "$(gcc -O2 -dM -E prog.c | grep -c __OPTIMIZE__)" = 1
gcc -O0 -g prog.c -o prog0 && gcc -O2 -g prog.c -o prog2
./prog0 && ./prog2
readelf -S prog2 | grep -q '\.debug_info'
gcc -O2 -ffast-math -dM -E prog.c | grep -q '__FAST_MATH__'
```

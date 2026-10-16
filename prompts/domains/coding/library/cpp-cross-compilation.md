trigger: when code must build for a different machine, or a build works locally and fails for the target
tags: #cpp #build #tooling
weight: 35
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html

Cross-compiling is deciding the *target* separately from the *host*, and being
honest about the difference everywhere a program is run.

- **A CMake toolchain file** (`-DCMAKE_TOOLCHAIN_FILE=`) is where the compiler,
  sysroot, and target triple are stated once: `CMAKE_SYSTEM_NAME`,
  `CMAKE_SYSTEM_PROCESSOR`, `CMAKE_C_COMPILER`, and usually
  `CMAKE_FIND_ROOT_PATH` so host libraries cannot leak in.
- **The sysroot is the whole world for the compiler** — headers and libraries for
  the target. A build that "accidentally" works usually found the host's.
- **`CMAKE_CROSSCOMPILING` is the flag for the rest**: anything that runs a program
  at configure time (a code generator, a test) needs `try_run`'s
  `CROSSCOMPILING_EMULATOR` (e.g. `qemu-aarch64`) or a build-time decision.
- **Tests for the target need an emulator** — `CTest` with a custom
  `CMAKE_CROSSCOMPILING_EMULATOR` runs the target binaries under qemu; a suite
  that cannot be run is a suite that is not run.
- **Static linking is usually what you want** for a cross-built artefact: the
  target may not have your host's shared library versions at all.
- **Check the artefact, not the build log**: `file` and `readelf -h` on the result
  say the architecture and the interpreter; a build with the wrong triple "succeeds"
  and produces something the target cannot execute.

## Run it

```run
# manual: needs a cross toolchain and a sysroot; the shape is
# `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux.cmake`
g++ -dumpmachine
```

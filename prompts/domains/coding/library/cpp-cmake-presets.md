trigger: when every developer types a different cmake invocation, or CI and local builds disagree
tags: #cpp #cmake #build
weight: 45
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html

A preset is a named configuration the repository owns, so the flags live in one
file instead of in everyone's shell history.

- **`configurePresets` for the cache variables**, `buildPresets` for targets,
  `testPresets` for the test invocation — `cmake --preset dev`, `cmake --build
  --preset dev`, `ctest --preset dev`.
- **`inherits` is the point**: a `base` preset holds the compiler and the
  generator, `dev` adds `CMAKE_BUILD_TYPE=Debug` and `CMAKE_EXPORT_COMPILE_COMMANDS`
  (which is what clangd and clang-tidy need), `release` adds `-O2 -DNDEBUG`.
- **`cacheVariables` beat `-D` on the command line** because they are reviewable
  and identical for everyone, including CI.
- **A `ci` preset exists and CI calls it**, so a red build is reproducible with one
  command locally. A pipeline that passes flags nobody can reproduce is a pipeline
  people learn to ignore.
- **`CMakeUserPresets.json` for machine-local paths** (a compiler in a non-standard
  place); it is gitignored by convention, and the shared file must not depend on it.
- **Version the file** (`"version": 3` for CMake 3.21+); an old CMake refuses a
  newer preset with a clear error, which is better than half-applying it.

## Run it

```run
mkdir -p pre
cat > pre/CMakeLists.txt <<'SRC'
cmake_minimum_required(VERSION 3.21)
project(presets CXX)
add_executable(main main.cpp)
SRC
cat > pre/main.cpp <<'SRC'
#include <cstdio>
int main() { std::puts("preset build ran"); }
SRC
cat > pre/CMakePresets.json <<'SRC'
{
  "version": 3,
  "configurePresets": [
    { "name": "dev", "generator": "Unix Makefiles",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } }
  ],
  "buildPresets": [ { "name": "dev", "configurePreset": "dev" } ]
}
SRC
cd pre && cmake --preset dev > /dev/null && cmake --build --preset dev > /dev/null && ./build/main | grep -q "preset build ran"
```

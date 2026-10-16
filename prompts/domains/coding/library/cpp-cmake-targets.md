trigger: when CMake behaves like a scripting language instead of a build system
tags: #cpp #cmake #build
weight: 65
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html

CMake is a language for describing *targets* and how they depend on each other.
Everything that goes wrong usually comes from treating it as a list of commands.

- **One target per artefact**, and every property set *on the target*:
  `target_link_libraries`, `target_include_directories`, `target_compile_options`.
  The old directory-scoped `include_directories` leaks into unrelated targets.
- **PUBLIC / PRIVATE / INTERFACE is the whole design.** PRIVATE is this target's
  own needs; INTERFACE is what consumers need; PUBLIC is both. A missing
  include directory in a consumer is almost always PRIVATE where PUBLIC was meant.
- **Generator expressions** (`$<...>`) are how a property differs per
  configuration or per compiler; they are evaluated at generate time, so a
  `message()` around one prints nothing useful.
- **Presets** (`CMakePresets.json`) are how a project states its real build
  configurations, so nobody has to remember the flags.
- **Out-of-tree always**: `cmake -S . -B build && cmake --build build -j`. A build
  directory inside the source is how `git status` noise starts.

## Run it

```run
mkdir -p demo/src
cat > demo/CMakeLists.txt <<'SRC'
cmake_minimum_required(VERSION 3.16)
project(demo CXX)
add_library(greet_lib lib.cpp)
target_include_directories(greet_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_executable(greet src/main.cpp)
target_link_libraries(greet PRIVATE greet_lib)
SRC
cat > demo/lib.cpp <<'SRC'
#include <cstdio>
void greet() { std::puts("hello from a public include"); }
SRC
cat > demo/src/main.cpp <<'SRC'
void greet();
int main() { greet(); }
SRC
cmake -S demo -B demo/build > /dev/null
cmake --build demo/build > /dev/null
./demo/build/greet | grep -q "hello from a public include"
```

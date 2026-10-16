trigger: when a library has to be consumed by something else, or a package build fails on paths
tags: #cpp #packaging #build
weight: 40
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://cmake.org/cmake/help/latest/manual/cmake-packages.7.html

Building in-tree is not shipping. Installation is a second set of decisions, and
they are the ones a consumer meets first.

- **`install(TARGETS ... EXPORT ...)` plus `install(EXPORT ...)`** is what makes
  `find_package(foo)` work for the next project; a CMake library that cannot be
  found by `find_package` is only usable from its own build tree.
- **`GNUInstallDirs`** (`CMAKE_INSTALL_LIBDIR`, `CMAKE_INSTALL_INCLUDEDIR`) puts
  files where the distribution expects them — `lib` on one distro, `lib64` or
  `lib/x86_64-linux-gnu` on another.
- **`SOVERSION` is the ABI promise** and produces the symlink chain
  (`libfoo.so -> libfoo.so.1 -> libfoo.so.1.2.3`). Changing the ABI without
  bumping the major version is how a consumer crashes after a routine upgrade.
- **`configure_file` for a `pkg-config` `.pc`** so non-CMake consumers can find
  the library; it is two variables (includedir, libdir) and a version.
- **`CPack`** turns the install rules into `.deb`/`.rpm`/`.tar.gz` without a second
  build description — the install tree is the package.
- **Test the installed artefact, not the build tree**: install to a scratch prefix
  and build a consumer against it in CI. A header that was never installed is the
  most common release-day failure.

## Run it

```run
mkdir -p pkg/src
cat > pkg/CMakeLists.txt <<'SRC'
cmake_minimum_required(VERSION 3.16)
project(greet CXX)
add_library(greet SHARED greet.cpp)
target_include_directories(greet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(greet PROPERTIES SOVERSION 1 VERSION 1.2.3)
install(TARGETS greet LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES greet.hpp DESTINATION include)
SRC
cat > pkg/greet.cpp <<'SRC'
#include "greet.hpp"
const char* greeting() { return "installed"; }
SRC
cat > pkg/greet.hpp <<'SRC'
const char* greeting();
SRC
cmake -S pkg -B pkg/build -DCMAKE_INSTALL_PREFIX="$PWD/pkg/prefix" > /dev/null
cmake --build pkg/build > /dev/null
cmake --install pkg/build > /dev/null
test -f pkg/prefix/include/greet.hpp && test -f pkg/prefix/lib/libgreet.so.1.2.3
test -L pkg/prefix/lib/libgreet.so
```

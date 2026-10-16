trigger: when a header is included everywhere and the build pays for it
tags: #cpp #build
weight: 35
verified: 2026-10
reference: https://en.cppreference.com/w/cpp/language/modules

Modules replace textual inclusion with an importable interface: the compiler
parses `import` once, in the right order, and macros stop leaking between files.

- **`export module name;` declares the interface**, `export` marks what the
  importer sees; anything not exported is invisible, which is the point.
- **Macros do not cross a module boundary.** A module cannot export one; a
  consumer that needs a configuration macro gets it from the build, not from the
  header. This is the migration's main source of pain.
- **A header unit** (`import "header.h";`) wraps an existing header, which is how
  a project adopts modules incrementally rather than in one release.
- **The build system must know**: modules need a BIMI/ifc dependency graph, so
  CMake 3.28+ (`FILE_SET CXX_MODULES`) or a recent Meson is required. Toolchains
  are years ahead of build systems here.
- **Expect to keep headers for the public ABI** until consumers move too; mixing
  is supported, and it is how a library ships both.

## Run it

```run
# manual: needs a build system with module support (e.g. cmake 3.28+ with FILE_SET CXX_MODULES)
echo "see the reference for the CMake target property"
```

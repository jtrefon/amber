trigger: when deciding how a library is linked, or a symbol is unexpectedly visible or missing
tags: #cpp #build #abi
weight: 45
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://gcc.gnu.org/wiki/Visibility

The linking model decides who pays the load, who sees the symbols, and what breaks
when the library changes.

- **Static** (`libfoo.a`): the code lands in the executable. Simplest to deploy,
  duplicated in every binary, and an ABI change means a rebuild of consumers.
- **Shared** (`libfoo.so`): one copy at runtime, and an ABI promise. The
  `SONAME` is that promise — `libfoo.so.1` — and it is why a rebuild that changes
  layout needs a new major number rather than a bug fix release.
- **Hide by default**: compile with `-fvisibility=hidden` and mark the public API
  `__attribute__((visibility("default")))`. Then every exported symbol is a
  decision, and the library loads faster because there is less to relocate.
- **Link order matters for static libraries**: the linker resolves left to right,
  so a consumer must come before the library that defines what it needs, and two
  libraries that need each other are a link error (`--start-group` exists to
  paper over a design that is usually circular).
- **`--as-needed`** (the distro default) drops libraries nothing referenced,
  which is why "it links on my machine" can differ from the package build.
- **`dlopen`/`dlsym` for plugins**, but pass an interface made of C or an abstract
  base declared identically on both sides; a C++ type crossing a `dlopen` boundary
  is the ABI problem with no build system to catch it.

## Run it

```run
cat > hidden.cpp <<'SRC'
__attribute__((visibility("default"))) int exported() { return 1; }
int internal() { return 2; }
SRC
g++ -std=c++17 -fPIC -shared -fvisibility=hidden hidden.cpp -o libhidden.so
nm -DC --defined-only libhidden.so | grep -q "exported()"
if nm -DC --defined-only libhidden.so | grep -q "internal()"; then exit 1; fi
```

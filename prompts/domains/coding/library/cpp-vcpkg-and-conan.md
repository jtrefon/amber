trigger: when a C++ project needs a dependency nobody has installed, or two projects need different versions
tags: #cpp #packaging #vendoring
weight: 40
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://learn.microsoft.com/en-us/vcpkg/get_started/overview

C++ has no central package manager; it has two credible ones and a set of
practices that matter more than the choice.

- **Pick what the project already uses.** Two mechanisms in one tree is a build
  nobody can reproduce, which is worse than either mechanism alone.
- **Manifest mode, not global installs**: `vcpkg.json` or `conanfile.txt` in the
  repository describes the dependency and its version, so a clone builds the same
  tree. A globally installed package is a fact about *your* machine.
- **Lock the versions**: vcpkg's baseline (`builtin-baseline`), Conan's lockfile.
  A version range is a different build tomorrow, and "it worked yesterday" is the
  symptom.
- **vcpkg triplets, Conan profiles** are how the same dependency is fetched for
  x64-linux, arm64, static or shared. That is the layer people forget until
  cross-compiling.
- **Binary caching** is why either is usable at scale: vcpkg's binary cache or
  Conan's remote, keyed by the package's hash and settings.
- **Vendoring is a legitimate answer** — `FetchContent` or a `third_party/` copy
  with the licence — when the dependency is one header or the build must work
  offline. Pin it to a commit, never a branch.
- **Check the licence once**, when adding it; nobody checks it again.

## Run it

```run
# manual: needs network access and a package manager; the shape is
# `vcpkg install --triplet x64-linux` or `conan install . --build=missing`
echo "describe the dependency in the repository, not on the machine"
```

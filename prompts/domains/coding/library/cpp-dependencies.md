trigger: when adding a library, or when a build must work somewhere else
tags: #cpp #packaging #vendoring
verified: 2026-10
sources: model knowledge 2026-10; not yet reviewed against the guides
weight: 45
reference: none

A dependency is code you did not write, you will not read, and you now maintain
the consequences of. Before adding one:

- **Is it already there?** The standard library, or a dependency the project
  already has, covers more than it looks like.
- **Who fetches it, and how?** Vendored or `FetchContent` keeps the build
  reproducible without a package manager; `conan` / `vcpkg` centralise versions
  but add a toolchain everyone must have. Follow what the project already does —
  a second mechanism in one tree is a build nobody can reproduce.
- **What does it drag in?** One library for one function is a poor trade; one
  library for the whole job is a good one.
- **Pin it.** A version range is a different build tomorrow, and "works on my
  machine" is usually this.

When a build fails only elsewhere, the difference is the environment, not the
code: reproduce with the same toolchain version and flags before changing
anything.

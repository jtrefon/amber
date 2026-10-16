name: the build and test loop
trigger: when a change needs building or testing and the project does not document how
tags: #cpp #cmake #make #testing
weight: 40
reference: none

Get one command that builds and one that runs the tests, and use them before and
after every change — a loop you cannot run is a loop you will guess in.

- A `Makefile` is the project's own interface: read its targets before inventing
  a command. `make -n <target>` prints what it would do without doing it.
- CMake projects build out of tree (`cmake -S . -B build && cmake --build build`
  with a few jobs). Reading `CMakeLists.txt` tells you whether the build wants
  options, a preset file, or a toolchain.
- Build the single target you touched, not the world: the edit-build-test loop
  is the thing you will run fifty times.
- When a build fails with a stale object or a layout change, clean that target —
  and if it persists, `make clean` once rather than arguing with the build.

Pass `-j` with the machine's real core count. A serial build of a large tree is
the most common way an agent wastes its own feedback loop.

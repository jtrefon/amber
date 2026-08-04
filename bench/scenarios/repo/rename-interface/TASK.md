# Task: cross-file refactor

The project (lib.h, lib.c, util.h, util.c) exposes a function named
`compute_value`. Rename it to `compute_result` **everywhere** — declaration,
definition, and every call site across all files — without changing any
behavior. The project must still compile.

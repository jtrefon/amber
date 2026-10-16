trigger: when a warning appears, or a rule is being argued about instead of checked
tags: #cpp #lint #tooling
verified: 2026-10
reference: none

The compiler is the reviewer that never gets tired. Treat its output as findings
rather than noise:

- Turn warnings on and keep them on (`-Wall -Wextra`, and the project's own
  flags from its build). A warning suppressed in a source file is a rule the
  next reader cannot see.
- `clang-tidy` and `cppcheck` find what the compiler leaves: shadowed variables,
  slicing, useless calls, unchecked optionals. Run them on the files you touched
  rather than the tree. If the project has a findings baseline, the rule is that
  it does not grow.
- A suppression is a decision: put the reason beside it, so the next reader
  knows what was traded away.
- When two tools disagree about a construct, that disagreement is a signal about
  readability — the compiler is not the only reader to be confused by it.

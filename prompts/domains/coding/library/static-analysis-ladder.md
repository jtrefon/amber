trigger: when a new warning or finding appears and the answer is fix, suppress, or silence the tool
tags: #lint #tooling
weight: 55
verified: 2026-10
reference: https://clang.llvm.org/extra/clang-tidy/
sources: Danilov, Refactoring with C++ (2024), Ch. 9-10; Eland, Refactoring with C# (2023), Ch. 12; pkglog "Static Analysis in C++" (2026); PHPStan baseline docs; golangci-lint docs; Clippy docs

Climb in cost order, and stop at the first rung that explains the finding.

1. **Compiler warnings** — the cheapest rung and the one closest to the truth
   of the code: `-Wall -Wextra -Werror` (or `/W4 /WX`), `rustc` lints. A
   warning names a path the program can take, so fix it or write down why it
   cannot happen. Building with a newer compiler version is a free extra pass
   over the same sources, without changing the production toolchain.
2. **Formatter** — clang-format, `gofmt`/`gofumpt`, `rustfmt`, Prettier or
   Biome format, `ruff format`, `dotnet format`, PHP-CS-Fixer. Formatting ends
   style debates mechanically and no lint rule should be doing its job.
3. **Linter** — per-file and syntax-aware: clang-tidy (fed by
   `compile_commands.json`, run on the files you touched), cppcheck (its own
   parser finds a different set), ESLint or Biome, `ruff check`,
   PHP_CodeSniffer and phpmd, the Roslyn analyzers and StyleCop (they ride the
   build), `golangci-lint` (which bundles `go vet` and `staticcheck`),
   `cargo clippy`. Disable a noisy check by name rather than weakening the
   whole configuration.
4. **Type checker** — reasons across files: `tsc --noEmit` with type-aware
   lint rules, mypy or pyright in strict mode, PHPStan or Psalm levels, the
   C++ compiler's own type system. This rung catches what a single-file linter
   provably cannot see.
5. **Deep analyser** — whole-tree and expensive: the Clang static analyzer
   (`scan-build`), `gcc -fanalyzer`, IWYU for include hygiene, Psalm's taint
   mode, Semgrep or CodeQL, SonarQube or PVS-Studio. Schedule it or make it
   advisory; its false positives are part of its price.

Two rules keep the ladder from collapsing under its own output:

- A finding is **fixed or suppressed with a reason** beside it, naming the
  check: `// NOLINT(bugprone-use-after-move): ...`, `# noqa: F401`,
  `#[allow(clippy::too_many_arguments)]`. A bare blanket suppression is a
  decision no reviewer can evaluate.
- A **baseline may only shrink** — phpstan-baseline, a warnings-as-errors
  allow list, golangci-lint's new-issues mode. Regenerating it to absorb what
  you just introduced turns a debt ledger into a hiding place.

Point every rung at your own sources: third-party headers and vendored trees
produce findings you must not fix, and they bury the ones you must.

## Run it

```run
printf '#include <stdlib.h>\nint main(void){int *p=malloc(sizeof *p);if(!p)return 1;int leftover=0;return *p;}\n' > warn.c
gcc -Wall -Wextra -c warn.c -o /dev/null 2> warnings.txt
grep -q "unused variable" warnings.txt
! gcc -Wall -Wextra -Werror -c warn.c -o /dev/null 2>/dev/null
printf '#include <stdlib.h>\nvoid f(void){char *p=malloc(4);free(p);free(p);}\nint main(void){f();return 0;}\n' > deep.c
gcc -fanalyzer -c deep.c -o /dev/null 2> analysis.txt || true
grep -q "double-free" analysis.txt
```

# Library buildout — tracker

State of the work described in `docs/spec/domains/coding-library.md` and the
owner's plan (`~/.commandcode/plans/library-depth-buildout.md`). Written to be
read by a fresh session: where the work is, what the contract is, and how to avoid
the two mistakes that cost time today.

## Where it is

| | |
|---|---|
| Mechanism | **merged** (#261): ranking, topic trees, closed tag vocabulary, `library_lookup`, and `make library-check` executing entries' `## Run it` blocks |
| Project index | **merged** (#260): private-state file, frozen block with provenance, `index_init/add/update/remove/show`, `/index show|refresh`, compaction refresh, redaction, append log |
| Content | **on `feat/coding-content`** (PR #262): 42 entries — 34 C++, 8 git — 25 blocks executing in `make check`, 9 declared manual |
| Selection | precision tests (a Rust profile must not pull the C++ toolchain; a Django+compose profile must not leak either) and a report of every vocabulary tag that selects nothing |

## The entry contract

One concept per file, in `prompts/domains/coding/library/` (topics are
directories with a `topic.md`). Header:

```
trigger: <when this applies — the line the working index renders>
tags: <from prompts/domains/coding/library/tags.md, and nowhere else>
weight: <0-100, heavier survives the 25-line cap first>
verified: <YYYY-MM>
reference: <the canonical doc>
sources: <book and chapter | article | "model knowledge <YYYY-MM>">
```

`## Run it` blocks (` ```run `) are executed by `make library-check` in a scratch
directory: a block that fails is **fixed**, never marked manual, and when a block
proves the entry wrong the entry is corrected. `# manual:` is for knowledge that
genuinely needs a cluster, a network or a cross toolchain.

Research is part of the contract: read the relevant chapter in
`/home/jack/Documents/SoftwareEngineeringBooks/` and do a web pass before writing
a set, then name what was used in `sources:`. The debt register is
`grep -rl "not yet reviewed" prompts/domains/coding/library`. `library-check` also
prints the vocabulary tags with no entry — today that list is the priority order.

## Next sets

1. Clear the remaining debt markers: *Pro Git* Ch. 7.7 Reset Demystified, 7.11
   Submodules, and the `git-worktree` man page.
2. C2 continued: git collaboration, `gh` beyond `pr create`, GitHub Actions,
   Azure DevOps.
3. C3 Docker and Compose — turns the two silent tags (`docker`, `dockercompose`)
   into output.
4. The languages the scanner detects and the library ignores — Python
   (django/fastapi/flask/pytest), TypeScript/Node (react/next/vue/angular/svelte/
   tailwind/vitest/jest), PHP (composer/laravel/symfony/wordpress/woocommerce/
   phpunit), .NET (dotnet/efcore), then Go and Rust depth.
5. Kubernetes, RDBMS, NoSQL; then back to C++ depth — debugger scripting,
   benchmarking, LTO/PGO, build caching, the standard-version strategy.

## Operating lessons (both cost real time today)

- **The checkout is shared.** Another session's commits landed on this branch
  between mine (`a955f9e` a RED test, `d4fd69f` its service), which failed every
  build in #262. Fix: before staging, `git log --oneline origin/main..HEAD` and
  `git status`; stage **explicit paths**; never `git add Makefile.in` wholesale
  while another session is editing it. When a branch has picked up foreign
  commits, rebuild it: backup branch, `checkout -B <branch> origin/main`, then
  cherry-pick only your own commits.
- **A stale binary lies.** A test failure after a branch move was a binary built
  from the other branch's source, not a real failure. Fix: `rm -f` the test object
  and rebuild before believing a red, and gate commits on a genuine verdict —
  `./run_tests | tail -1 | grep -q "0 failed"` — never on a pipe's exit status.
- **Regenerate after switching:** `Makefile` is generated from `Makefile.in`;
  a stale one can point at files that no longer exist (`./configure` fixes it).

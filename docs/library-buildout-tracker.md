# Library buildout — tracker

State of amber's coding library and the work around it. Written for a fresh
session — human or scheduled — to resume without re-deriving anything.

## Where it is

| | |
|---|---|
| Mechanism | **merged** (#261): ranking, topic trees, closed tag vocabulary, `library_lookup`, `make library-check` executing entries' `## Run it` blocks |
| Project index | **merged** (#260): private-state file, frozen block with provenance, `index_init/add/update/remove/show`, `/index show|refresh`, compaction refresh, redaction, append log |
| Content | **56 entries**, 31 blocks executing in `make check`, 17 declared manual, 0 failing — C++ (34), git (8), Docker/Compose (3), and the craft layers (11) |
| Accuracy | profile tests for a Rust CLI, a Python+compose app, a .NET+Angular monorepo, a Go service, and a C++/CMake+compose project; `library-check` prints the vocabulary tags no entry carries |

**Open PRs (all green or running, all the owner's to merge):** #265 profile
accuracy + the `#cpp` scanner fix · #266 Docker/Compose · #267 the craft layers.

## The entry contract

One concept per file in `prompts/domains/coding/library/` (a topic is a directory
with `topic.md`). Header, all fields enforced by tests:

```
trigger: <when this applies — a situation, not a topic>
tags: <from prompts/domains/coding/library/tags.md, the closed list>
weight: <0-100; heavier survives the 25-line cap first>
verified: <YYYY-MM>
reference: <the canonical doc>
sources: <book + chapter | article | "model knowledge <YYYY-MM>">
```

`## Run it` blocks (```run) are executed by `make library-check` in a scratch
directory. A block that fails is **fixed**, not marked manual; a block that
disproves the entry means the entry is wrong and gets corrected. `# manual:` is
for knowledge that genuinely needs a cluster, a daemon or a network.

## How sets are written (the pattern that works)

Research first, in parallel, by subagents — each reads the relevant chapters of
`/home/jack/Documents/SoftwareEngineeringBooks/` (453 titles; PDF/EPUB extract to
Markdown), does a web pass for current practice, writes its own entries, runs
`library-check` on them, and reports a six-line summary. Four agents produced the
eleven craft entries in one round without loading this conversation's context.
**No model inference is needed for any of this**: it is static content plus
deterministic tests. (The local inference service is out of scope and is not
touched — earlier planning that assumed a live session was a mistake.)

## The two kinds of gap

1. **Silent scanner tags** — the scanner proposes them, no entry carries them, so
   the working index stays empty. `library-check` prints this list every run.
   Today: angular bootstrap composer django dotnet efcore fastapi flask go jest
   laravel next node php phpunit pytest python react svelte symfony tailwind
   typescript vitest vue woocommerce wordpress.
2. **Depth with no scanner tag** — the craft layers are reached when an agent adds
   `#design`, `#patterns` or `#architecture` to a project index, or asks
   `library_lookup`. They never move list 1; they are what makes the agent an
   engineer rather than a tool-runner.

**Debt register:** `grep -rl "not yet reviewed" prompts/domains/coding/library`
(the Docker set awaits Kane, *Docker: Up and Running* 3ed).

## Next sets, in order

1. **Docker research pass** — Kane 3ed, clearing that debt register entry.
2. **TypeScript/Node** — nine silent tags, the largest remaining gap (react, next,
   vue, angular, svelte, tailwind, bootstrap, vitest, jest), then **Python**
   (django/fastapi/flask/pytest), **PHP** (composer/laravel/symfony/wordpress/
   woocommerce/phpunit), **.NET** (dotnet/efcore), **Go** and Rust depth.
3. **Kubernetes, RDBMS (Postgres first — the shelf has three Postgres books),
   NoSQL/streaming.**
4. **More C++ depth** — debugger scripting, benchmarking, LTO/PGO in practice,
   build caching, the standard-version strategy.

## Operating lessons (each cost real time)

- **The checkout is shared.** Another session's commits landed on a branch of
  mine, and my `git add Makefile.in` swept their line into my commit, failing
  every build in #262. Before staging: `git log --oneline origin/main..HEAD` and
  `git status`; stage explicit paths; when a branch has picked up foreign work,
  rebuild it (backup branch, `checkout -B <branch> origin/main`, cherry-pick only
  your own commits).
- **A stale binary lies.** A test failure after a branch move came from a binary
  built on the other branch. Rebuild before believing a red, and gate a commit on
  `./run_tests | tail -1 | grep -q "0 failed"` — never on a pipe's exit status.
  (`Makefile` is generated; `./configure` after switching branches.)
- **Stashes are the ref nobody misses.** Two are now permanent branches:
  `backup/stash-worktree-tests-2026-10-16` and
  `backup/stash-taste-toolset-audit-2026-10-16`.

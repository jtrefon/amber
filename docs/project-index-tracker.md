# Project index — phase tracker

Phases for `docs/spec/domains/project-index.md`. Written to be picked up by a
fresh context: each phase names its deliverable, files, tests and exit criteria.

**How to resume here.** Repo rules that matter for this work: `main` is
PR-only; work happens on a feature branch; behaviour changes arrive red first
(failing test, then implement); gates are `make test` (unit + bench), `make
check` (build hygiene and the gate self-tests), `make lint-changed
BASE=origin/main`, `make analyze`, and clang-format **18.1.3** (the pinned CI
version — a newer local clang-format disagrees with CI). The template for PI-1 is
`plugins/project_instructions/` (read-once file, cached render, disable removes
the block) and the tests in `tests/plugin_runtime_test.cpp`.

## PI-1 — the index file and its block (small)

**Deliverable.** `plugins/project_index/`: a plugin (category `domain`, so it
travels with the coding work for now) that
- resolves `<state>/projects/<slug>/project-index.md` (private state; never
  created implicitly by a read),
- contributes a System `PromptBlockCapability` whose render is **cached at
  activation** and re-read only on activation/explicit refresh,
- writes provenance (source paths + hashes) and renders the staleness line when a
  source has moved,
- exposes nothing else yet.

**Files.** `plugins/project_index/project_index_plugin.{h,cpp}`,
`Makefile.in`, `lib/plugins_bundled.cpp`, `include/agent/prompt.h` (a
`kProjectIndex` priority next to `kProjectInstructions`), tests in
`tests/plugin_runtime_test.cpp`.

**Tests.** The block appears when the file exists; is absent when it does not;
disabling the plugin removes it; a stale source renders the `changed since` line;
the render is byte-identical across two renders in one activation (the frozen
rule).

**Exit.** `make test`, `make check`, `make lint-changed` clean; the resident
budget test from PI-5's scope is not yet required, but the block is ≤ 30 lines.

## PI-2 — the toolset

**Deliverable.** `index_init`, `index_add`, `index_update`, `index_remove`,
`index_show` as `ToolCapability` contributions, plus a `/index show|refresh`
command. Decisions PI-D5 (approval: init gated; the rest free), PI-D6 (pointers
not values) and the append log are implemented here.

**Files.** the plugin, a small redaction helper, the tool schemas, tests.

**Tests.** `index_add` writes the file and returns the canonical entry text;
`index_remove` takes it out; a credential-shaped value is refused with a clear
message; the append log records each mutation; `index_show` reflects a change made
in-session while the prompt block does not (the frozen rule, again, from the tools'
side).

**Exit.** as PI-1, plus: a fresh session renders what the previous session added
(the "before the first prompt" behaviour, which is really just activation).

## PI-3 — the compaction refresh

**Deliverable.** the block re-renders when amber compacts the context, so a long
session picks up index changes without an explicit refresh.

**First: verify.** Find the compression site and the observer/event seam
(`ContextEventSource`, the compression pipeline's observer, the `EventBus` typed
events). If a plugin cannot subscribe safely, add a typed event at the compaction
site — that is a small, honest addition, not a redesign.

**Tests.** a compression that changes the block's inputs re-renders it; a
compression with unchanged inputs produces an identical block; no other path
mutates the block mid-session.

**Exit.** as before.

## PI-4 — library plumbing

**Deliverable.** the working index: the project index's tags select library
entries, whose trigger lines are injected per turn (the skill-discovery slot), and
whose bodies load on demand (`read_skill`). The library is **bundled skills** in
the coding domain (decision PI-D7), so enable/disable/block and the discovery
mechanism come for free; this phase is the tag→entry selection and the cap.

**Tests.** tags select exactly the matching entries; the working index is capped
and stays out of the system prefix (per-turn block, KV-safe); a disabled coding
domain leaves no index line behind.

**Exit.** as before, plus the KV check: two renders of the system prompt with
different working-index contents are byte-identical.

## PI-5 — library content

**Deliverable.** the content itself, per `docs/spec/domains/coding-library.md`:
one file per concept, trigger first line, tags, reference. Seeding order: **C++
first** (amber is itself the dogfooding case), then Rust, PHP, C#, TS, Python.

**Also here:** the resident-budget test (PI-D9) and the one-home scan (no library
entry restates a rule that lives in core, a tool doc or `AGENTS.md`).

**Exit.** the gates, plus a session that uses the index end to end on a real
project.

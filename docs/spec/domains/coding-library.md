# The coding library

The content behind `docs/spec/domains/project-index.md`: what amber's coding
domain knows, how it is organised, and what earns a place in the resident prompt
rather than the index.

## Residency, in one rule

**Residency is for the key, not the library.** A line is resident when it applies
on most coding turns *and* getting it wrong is expensive. A line is indexed (one
trigger-phrased line per turn, body on demand) when the agent must know it exists
but needs it in a minority of turns. Everything else is **dark**: reachable by
reading the project, searching, or asking — which is the behaviour the core prompt
already demands. Project-declared facts (`AGENTS.md`, the project index) outrank
all three.

Budget: the coding domain's resident prose — identity, workflow, fundamentals,
paradigm, project index — stays within **110 content lines**, asserted by a test.
The number is the measured value (108 when it was pinned), ratcheted like every
other budget in this repo: it may only shrink. The library has no budget: it is
dark.

## Retrieval

The working index is the map, `library_lookup` is the route.

- The **craft layer** (`design`, `patterns`, `architecture`) is indexed for every
  project. It answers what the *task* needs, and a project tag cannot select it;
  a fresh project with no index still sees it. The project's own tags add the
  domain entries. The block is capped at 25 lines, and a trailing `(N more
  selected entries)` line says when it truncated.
- A query matches **whole words** and, for words of four characters or more,
  prefixes. A tag's `also:` synonyms and an entry's own `also:` phrases match
  too, so "OOP" reaches every design entry and "event sourcing" reaches the
  event-driven one. Name and trigger hits outrank body hits, and a single body
  mention is not enough to return an entry.
- `library_lookup(list=true)` — and `/library list` — prints the vocabulary and
  every entry. A miss names the vocabulary instead of returning silence.
- The coding domain contributes **one** `coding-library` skill: the inventory
  line in the discovery slot, with the vocabulary as its body. It is not one
  skill per entry — the skills catalog has no tags, a 20-line discovery cap and a
  body budget, and per-entry skills would invert the economics that make the
  library cheap.

## The layers

| Layer | State | Content |
|---|---|---|
| **L0 Fundamentals** | resident (short) + indexed depth | cohesion, dependency direction, least structure that works, errors at the caller's level, tests as proof, change size |
| **L1 Paradigm** | resident one-liner + indexed | OOP (SOLID as answerable questions, composition, patterns) · functional (purity, immutability, effects at the edges) · systems (ownership, lifetimes, allocation) |
| **L2 Architecture** | indexed | layered, hexagonal, onion, clean, event-driven, CQRS, monolith↔services — each entry trigger-phrased as *"when placing a new component"*, *"when a module reaches across a boundary"* |
| **L3 Craft** | indexed | naming and function shape, comment discipline, refactoring moves, **legacy code: characterize before changing**, reading your own diff as a reviewer, **test design** (what makes a test a specification, hermeticity, doubles, property tests) |
| **L4 Language** | digest resident (≤ 5 lines per language, at most two languages) + indexed depth | per stack: idioms that bite, build/test tooling, static analysis, debugger, packaging/vendoring, concurrency and cancellation model |
| **L5 Framework** | indexed, project-declared first | PHP: Laravel, Symfony, WordPress/Woo · C#: ASP.NET, EF Core · TS: React, Next · Python: Django, FastAPI · with *"the framework's opinion is the default; the project's AGENTS.md and project index win"* |
| **L6 Cross-cutting** | indexed | data and migrations, API contracts and versioning, security review, performance (measure before changing), **runtime observability**, dependency hygiene, build/CI divergence, release and versioning |

## Per-language checklist (L4)

Every language entry set covers the same ground, because the agent needs the same
decision support in each: **idioms that bite** (the few that change design, not a
language tour), **build and test** (commands and their traps), **static
analysis/lint** (the project's own gate matters most), **debugger**, **packaging
and vendoring**, **concurrency/cancellation**, and **runtime telemetry** (how to
see the app while it runs).

Seeded order: **C++** (amber itself — instant dogfooding: CMake/Make, gcc/clang,
sanitizers, gdb/lldb, conan/vcpkg), then **Rust** (cargo, clippy, rustfmt, tokio
ownership and cancellation, `rust-gdb`), **PHP** (composer, PSR, phpunit, phpmd,
xdebug; Laravel/Symfony/WordPress per L5), **C#** (.NET SDK, xUnit, analyzers,
NuGet, EF Core), **TS** (tsconfig strict, ESLint/Biome, vitest/jest, node
debugging), **Python** (uv/venv, pytest, ruff/mypy, pdb).

## Entry rules

1. **One file, one concept.** The concept is the unit of retrieval, so it is the
   unit of authorship.
2. **The first line is the trigger**, written for matching a *situation*, not
   describing a *topic*: not "Design patterns" but *"when a shape repeats and the
   call sites are coupled — strategy, factory, adapter, observer"*.
3. **The index renders that line verbatim.** A concept cannot exist without
   declaring when it applies, and a file cannot drift from its index entry.
4. **Header** (shared with project-index entries):

   ```
   name: <short name>                 (optional: the file's stem is the name)
   trigger: <when this applies>
   tags: #dotnet #efcore
   also: <the names people use>       (optional: OOP, SOLID, event sourcing)
   weight: <0-100>                    (optional: 50 default, topics 60)
   verified: <YYYY-MM>
   reference: <url | path>
   sources: <book and chapter | article url | "model knowledge <YYYY-MM>">
   reference: <url | path>            (optional)
   ```

   `also:` is retrieval vocabulary, not selection: a tag's own line in
   `tags.md` may carry `also:` too, and every entry with that tag inherits the
   words. It exists because the reader's word ("OOP", "event sourcing") and the
   entry's word ("design", "event-driven") are often different, and a knowledge
   base that only answers in its own vocabulary is one nobody finds.

   `sources:` is provenance, not authority: it names what was actually consulted —
   a chapter of one of the guides in the owner's library, a current article, or
   explicitly model knowledge, which is the honest marker for an entry that has
   not been reviewed against a guide yet. Paraphrase, never paste: a reference
   points at the source of truth, it does not reproduce it.

5. **Entries earn their line.** An entry that is never pulled is a line that
   should be dark; pruning is manual in v1 and measured later (see the spec's open
   items).
6. **One home per rule.** An entry never restates a rule that lives in core, in a
   tool doc, in a domain block or in a project's `AGENTS.md` — it references the
   decision and adds what is specific. A scan test enforces this.

## TDD, as a position rather than a rule

Teach **test design** (L3): what makes a test a specification, what makes it
brittle, when a double is honest and when it hides the bug, what to assert about
behaviour versus implementation. The failing-test-first move is taught as a
*technique* for when the interface is the open question — not as a standing
requirement. The failure this targets is real and specific: agents write tests
that assert their own implementation, which passes, proves nothing, and freezes a
mistake in place.

## What is deliberately not here

- **Language tours.** The model knows the syntax; an entry exists only for what
  changes a decision.
- **Vendor documentation mirrors.** A `reference:` URL or path points at the
  source of truth instead of copying it.
- **Framework tutorials.** L5 entries describe the framework's *opinion* and where
  the project's own conventions override it.
- **Anything already stated elsewhere** — see rule 6.

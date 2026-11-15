# Plan: pre-1.0 production readiness

Status: **active**. No version stamp and no tag until the `pre-1.0 cleanup`
milestone is empty and the release checklist passes. Tagging is a separate,
explicit decision.

Decision log: 2026-11-15 — the 1.0 claim moves from "modular core with bundled
plugins" to a **decomposed core and a plugin-free core archive**. The work
previously deferred by #562 is now pre-1.0 cleanup.

## Why this exists

The 1.0 milestone itself has no open code child: #562 is the release gate and
every code child is closed. But the tracker still carries ten post-1.0 issues,
including two architecture refactors (#274, #563) that change what 1.0
promises. Shipping 1.0 with those open would make the release claim depend on
work the release explicitly deferred. This plan closes that gap: every open
issue is either implemented and closed, or the plan records why it is not part
of 1.0. The current answer is: all ten are part of 1.0.

## Current inventory (2026-11-15)

| Issue | What remains | PRs | Depends on |
|---|---|---:|---|
| #601 | CLI telemetry refactor; mock OpenAI server; `--driver binary`; five e2e scenarios; `bench-e2e` job | 1–2 | — |
| #596 | Benchmark epic; closes with #601 | — | #601 |
| #274 | `PromptAssembler`, `ToolDispatcher`, `CompressionController`, `Agent` as composition root | 4 | #596 |
| #563 | Host composition root, `libagent_plugins.a`, plugin-free build, remove `lib/ -> plugins/` exceptions | 3–4 | #274 |
| #445 | Multi-line input, block layout, history semantics, draft persistence, paste, toggle | 1–2 | — |
| #381 | `RequestClass`, admission/aging, call sites, `/throttle` counts, tests | 1–2 | #274 |
| #441 | Checkpoint refs/shadow repo, turn trigger, `/history` `/undo` `/rollback` `/diff`, agent tool | 3–4 | #274 |
| #440 | Profile storage, commands, picker/completion, project override, migration | 2–3 | #563 |
| #410 | skills.sh v1 adapter: token auth, v1 endpoints, hashes/audits, fallback | 1 | — |
| #391 | Skills epic; closes after #410 | — | #410 |
| #562 | Release execution: version stamp, formula, website, live bench, checklist, rc | 1 | all of the above |

Indicative size: 18–22 implementation PRs. At the current gate cadence this is
a multi-month program, not a single session. The estimates are for sequencing,
not commitments.

## Workstreams

### A — Quality baseline (first)

#601 completes the hermetic benchmark's binary tier, then #596 closes. This is
the regression net for the refactors: without it, #274/#563 would be measured
only by the live corpus and the unit suite.

Exit: `bench-e2e` gates `ci-gate`; the five installed-tree scenarios agree with
the in-process tool sequence; #601 and #596 closed.

### B — Core architecture

#274 first, one extraction per PR:

1. `PromptAssembler` — block ordering and source merging; byte-identical
   prompt fixture.
2. `ToolDispatcher` — `DispatchRequest` object, parameter count ≤6.
3. `CompressionController` — lifecycle/Memento entry points; pipeline stays.
4. `Agent` composition root — `IAgent` implementation, published snapshots,
   store accessors only.

Then #563: move the composition root out of `lib/`, split
`libagent_plugins.a`, make a plugin-free core build and install smoke real,
and remove the `lib/ -> plugins/` hygiene exceptions.

Exit: the hermetic bench is byte-identical per extraction; a plugin-free core
links and boots; packages still pass; #274 and #563 closed.

### C — Product surfaces

- **#445** (TUI input) is independent of the core and can run while B is in
  flight, but it shares the command tree/status bar with #441/#440. Assign
  those files to one stream at a time.
- **#381** (provider priority classes) touches the governor and HTTP transport,
  not the Agent; schedule it after #274 to avoid call-site churn.
- **#441** (git undo) needs a stable turn boundary; schedule after #274.
- **#440** (profiles) touches provider/config and the TUI; schedule after
  #563 so it targets the final composition root.
- **#410** (skills adapter) is small and independent; run it in any stream,
  then close #391.

Exit: #445, #381, #441, #440, #410 and #391 closed.

### D — Release

#562 only after A–C. Stamp `1.0.0`, bump the formula, replace the Beta copy,
produce the live 1.0 benchmark artifact, run the release checklist, cut an
`rc` rehearsal, and tag only with explicit approval. A tag publishes
unattended; the rc is the rehearsal, not a formality.

## Sequence

**Critical path:** #601 → #596 → #274 → #563 → #441 → #440 → #562.

**Parallel stream:** #445 (TUI) and #381 (governor) can run alongside B;
#410 can run anytime. If parallel sessions are not available, append them
after #563 in the order #445 → #381 → #441 → #440.

## Risks and mitigations

- **#274/#563 change the 1.0 claim.** The API compatibility note, README,
  website and #562 decision log must be updated in the same milestone. A
  plugin-free core changes how embedders link the runtime.
- **The benchmark baseline must land before #274.** A refactor without a
  hermetic baseline is not measurable; #601/#596 is the gate.
- **#441 depends on the turn boundary.** Doing it before #274 risks a second
  move of the same hooks. Do it after the composition root is stable.
- **#445/#440/#441 share the TUI command tree and status bar.** One stream at
  a time per file; otherwise the PRs conflict on the JSON-driven surface.
- **Release work is not the long pole.** The code is; the release checklist
  and live bench should not start until the cleanup milestone is empty.

## Definition of done

- [ ] `pre-1.0 cleanup` milestone is empty.
- [ ] The 1.0 milestone contains only #562, and #562's exit criteria pass.
- [ ] `make verify`, `make coverage BASE=origin/main`, `make check`,
      `make test` and `make pty-matrix` are clean on the release commit.
- [ ] The live 1.0 benchmark artifact exists and is within the release band.
- [ ] The installed-tree licence checks, apt/dnf/Homebrew installs and the
      0.8 → 1.0 upgrade are exercised.
- [ ] The website, README, `UPGRADING.md` and release notes tell one 1.0
      story.
- [ ] The `rc` tag run is green end to end; the final tag is explicitly
      approved.

## Decision log

- 2026-11-15: full backlog cleanup before 1.0. #274/#563 are no longer
  post-1.0; all ten open issues are in the `pre-1.0 cleanup` milestone. No
  version stamp or tag until that milestone is empty.

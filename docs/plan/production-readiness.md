# Plan: pre-1.0 production readiness

Status: **release execution**. The `pre-1.0 cleanup` milestone is empty
(0 open / 12 closed) as of 2026-11-20. No version stamp and no tag until the
#562 exit criteria pass. Tagging is a separate, explicit decision.

Decision log:
- 2026-11-15 — the 1.0 claim moves from "modular core with bundled plugins" to
  a **decomposed core and a plugin-free core archive**. The work previously
  deferred by #562 becomes pre-1.0 cleanup.
- 2026-11-20 — all cleanup work has landed. The plan now tracks release
  execution only.
- 2026-11-21 — the two remaining capability/usage issues closed: #618 landed
  as `05f9293` (reasoning tokens in `/usage`), #617 as `ccbf003` (one
  capability alias table, verbosity, Gemini `thinkingConfig`). One bug remains
  open in no milestone: #621, the macOS tab-switch freeze; it needs a repro or
  an explicit defer before the tag.

## Why this exists

The 1.0 milestone itself has no open code child: #562 is the release gate.
The pre-1.0 work that changed what 1.0 promises — the core decomposition,
plugin-free archive, checkpoints, priority classes, profiles, skills adapter,
input ergonomics — is all merged and closed. This plan records that state and
the release work that remains, so the release claim cannot silently depend on
work nobody finished.

## Cleanup inventory (closed 2026-11-20)

| Issue | Landed as | State |
|---|---|---|
| #601 / #596 | binary driver, JSON telemetry, hermetic benchmark epic | closed |
| #274 | `PromptAssembler`, `ToolDispatcher`, `CompressionController`, `Agent` composition root | closed |
| #563 | host composition root, `libagent_plugins.a`, plugin-free core | closed (#658) |
| #445 | multi-line composer, draft persistence, paste safety | closed (#624) |
| #381 | `RequestClass`, admission/aging, `/throttle` counts | closed (#659) |
| #441 | per-turn checkpoints, `/history` `/undo` `/rollback` `/diff`, agent tool | closed (#660, #668) |
| #440 | named provider profiles, project pin, instant switching | closed (#677) |
| #410 / #391 | skills.sh v1 adapter, curated index, audits, stable hashes | closed (#678) |
| #562 | release execution | open — this plan |

Two TUI fixes from the same cleanup window are also merged: #613's tab close
target and confirmation (#675, finished visually in #682), and Shift+Tab mode
cycling (#612, #676). The context-cache regression found during that work is
fixed in #681.

## What remains

### Release execution (#562)

1. Stamp `1.0.0`; verify `debian/changelog`, both `rpm/*.spec` and
   `homebrew/amber.rb` agree with `version.txt` via `tools/release_check.py`.
2. Replace beta copy in `website/src/pages/index.astro`,
   `website/src/pages/report.astro` and `website/src/components/AsciiHero.astro`.
3. Produce the live 1.0 benchmark artifact under `bench/results/releases/1.0.0/`
   and confirm it is inside the recorded band.
4. Execute `docs/release-checklist.md` end to end: local gates, rc tag
   rehearsal, Homebrew sha256 bump, installed-tree licence check, apt/dnf/Homebrew
   version agreement, and a 0.8 → 1.0 upgrade.
5. Tag only with explicit approval — a tag publishes unattended.

### Decisions before the tag

- #617 (P2, closed 2026-11-21): the reasoning contract landed in #623; Gemini
  `thinkingConfig`, verbosity and the generic field-alias table landed in
  #686 (`ccbf003`). No capability work remains.
- #618 (P3, closed 2026-11-21): reasoning tokens in `/usage` landed in #683
  (`05f9293`). No usage work remains.
- #621: macOS tab-switch freeze, no reproduction on Linux; journey guard is on
  `main`. Needs a repro or an explicit defer.
- #562's "formerly deferred" list and the plugin **Deferred Register** name
  PF-6 external plugins, themes, raw key interception, window geometry, hot
  reload and log sinks. All have documented reopen conditions and no consumer;
  the epic text still shows them unchecked as pre-1.0 work. Decide: out of 1.0
  by reference to the register, or in scope with a file issue.

## Exit criteria

- [x] `pre-1.0 cleanup` milestone is empty (done 2026-11-20).
- [ ] The 1.0 milestone contains only #562, and #562's exit criteria pass.
- [ ] `make verify`, `make coverage BASE=origin/main`, `make check`,
      `make test` and `make pty-matrix` are clean on the release commit.
- [ ] The live 1.0 benchmark artifact exists and is within the release band.
- [ ] The installed-tree licence checks, apt/dnf/Homebrew installs and the
      0.8 → 1.0 upgrade are exercised.
- [ ] The website, README, `UPGRADING.md` and release notes tell one 1.0
      story (no beta copy left).
- [ ] The `rc` tag run is green end to end; the final tag is explicitly
      approved.

## Risks and mitigations

- **The unmilestoned issues can be mistaken for non-blockers.** A green
  milestone gate says nothing about them. Resolve their 1.0 status explicitly
  before the tag rather than by silence.
- **The release claim changes with the plugin-free core.** The API
  compatibility note, README, website and #562 decision log must be updated in
  the same milestone.
- **Release work is not the long pole.** The code is done; the checklist, live
  bench and rc rehearsal should start before the final stamp, not after.
- **A tag publishes unattended.** The release environment has no reviewer gate;
  the version assertion and tag access control are the guard. Verify both.

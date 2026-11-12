# CI and engineering quality hardening — 2026-11 audit

Status: approved direction. Tracking epic: #579. Children: #569–#578.
This document is the shared context; each issue is self-contained for an
independent session.

## 1. Why this exists

Four incidents in one day showed that the gate set is stronger on **code
quality** than on **operational resilience** and **measurement depth**.

| Incident | Evidence | What it exposed |
|---|---|---|
| Ubuntu mirror DNS outage | Run `43089600275`: 13 apt-based jobs failed in `install-deps` with `Temporary failure resolving 'archive.ubuntu.com'`, exit 124 after the 30-minute bound; three attempts over hours. CodeQL, macOS and non-apt jobs passed. | CI depends on live mirrors and `ubuntu-latest`; no fallback, no pinned image, no dependency cache. |
| `ci-gate` queued for ~12h | Job `130782901252` queued at `20:35:09Z`; run marked failed at `21:01:34Z` while the job was still `queued`. A re-run queued behind it in the same concurrency group and also never started. | Queued jobs have no timeout; a zombie job holds the concurrency slot and blocks the only required check. Nothing alerts. |
| macOS timing flake on `main` | `FAIL: elapsed_ms(started) < 5000`, run `43084297324`; the same commit's PR run was green. Hotfix #568. | Wall-clock assertions are nondeterministic; no repeat run, quarantine or lint. |
| TSan race found late | `PluginInfo::state` read by the tool-call worker, written by `disable`; caught by the new toggle test. | The gate works, but concurrency paths are thinly tested; coverage does not imply interleaving coverage. |

## 2. Verdict

For code-level debt the enforcement is **above the norm**: fail-closed gates,
sentinels, gate self-tests, static and dynamic breadth, pinned analyzers and
supply-chain checks. It is **not enough to guarantee** zero debt or top
quality, and the residual risk is concentrated in:

- CI availability and liveness (the incidents above);
- nondeterministic tests;
- measurement depth (branches, fuzzing the newest parser, mutation);
- supply-chain inputs (mutable action tags, no SBOM/reproducibility check);
- human review capacity;
- agent efficiency (tokens/latency/tool calls are not gated).

## 3. What is already strong (do not rebuild)

- **Fail-closed gate integrity**: `lint-tidy-tu` sentinels + `--expect-tu`,
  parse-count floors, `MIN_FUNCTIONS_SANE`, analyzer version floors,
  `ci-manifest`, local-parity P15, 400+ gate self-tests.
- **Static**: clang-tidy ratchet, cppcheck 2.22, complexity (CCN/NLOC/PARAM),
  nesting, class size, zero-TODO debt, detach ratchet, duplicates, hygiene and
  header self-containment, format, obfuscation, gitleaks.
- **Dynamic**: unit + pty/e2e, both compilers with `Werror`, macOS,
  ASan/UBSan, TSan, three fuzz targets, line coverage floor + patch gate.
- **Supply chain**: pinned analyzers, md4c hashes, dependency review,
  provenance attestation, signed packages.
- **Process**: one PR per feature/bug (this document tightens it), strict
  `ci-gate`, protected `main`, merge train, test-presence.

## 4. Gap map

| Issue | Prio | Gap | Independent outcome |
|---|---|---|---|
| #569 | P0 | No CI liveness watchdog; a queued job can block the required check for hours; no red-main alert | Stuck runs self-recover or open one tracked issue |
| #570 | P0 | Apt install depends on live mirrors and a moving `ubuntu-latest` | Retries/fallback, pinned image/container, dependency cache |
| #571 | P0 | Timing tests flake; no repeat/quarantine/lint | A flake cannot reach `main` silently |
| #572 | P1 | Coverage is line-only (branches 44.2%) | Patch branch coverage, ratcheted |
| #573 | P1 | Plugin JSON-RPC parser is not fuzzed | Corpus + ASan/UBSan target |
| #574 | P1 | Actions pinned by tags; no SBOM/reproducibility check | SHA pins, SBOM, double-build compare |
| #575 | P1 | Plugin source compatibility is policy only | `make api-check` with a snapshot |
| #576 | P1 | Tokens/latency/tool calls have no baseline | Hermetic regression gate |
| #577 | P2 | No product-code mutation testing | Measured spike, then adopt or reject |
| #578 | P1 | PRs opened before completion overload review | One PR per finished feature/bug, manual E2E, review mechanism |

## 5. Execution order

1. **#569, #570, #571** — stop losing time to infrastructure and flakes. All
   three are independent and can run in parallel.
2. **#578 + this document's SDLC change** — make a PR mean “finished and
   reviewable”.
3. **#572, #573, #575** — deepen what is measured.
4. **#574, #576, #577** — supply chain, cost/performance, mutation.

Each issue lands as **one PR** when complete, with manual end-to-end evidence
in the body (see AGENTS.md, Development workflow).

## 6. How to work an issue in an independent session

1. Read the issue and this document; do not rely on conversation history.
2. Branch from `main`: `<type>/<short-issue-name>`.
3. Write the failing test first (red commit), then the implementation. Keep
   task/red/green/refactor steps as commits on the same branch.
4. Run `make verify-quick` before each push, `make verify` before review, and
   the coverage recipe when production lines change.
5. Perform the manual end-to-end check. Record steps, environment and
   expected/observed in the PR body.
6. Open the PR only when the issue's acceptance criteria are all met. Link the
   epic (#579) and the issue. Do not open a draft as a progress board.
7. Close the issue with the PR; update the epic checklist.

## 7. SDLC policy change (from #578)

- **Unit of delivery:** one completed feature or bug fix.
- **Unit of progress:** a commit (red test, green fix, refactor, docs).
- **PR timing:** only when the feature/bug is complete, `make verify` is clean,
  and the manual end-to-end test can be run and recorded. No per-task PRs, no
  draft PRs used as status boards.
- **Review:** feature/bug PRs are reviewed by the maintainer before merge.
  The repository has one collaborator and GitHub forbids self-approval, so the
  native required-review check cannot be satisfied by the author. #578 decides
  the mechanism (second reviewer identity, an enforced review label, or
  gate-only for docs/dependency PRs). Until it lands, record the review
  explicitly in the PR body.
- **Batching target:** at most one PR per finished feature/bug. A milestone may
  span several PRs, each opened at completion.
- **Measurement:** PRs per feature, review wait, flake rate, time-to-green,
  stuck-run count.

## 8. Non-goals

- Rebuilding gates that already work (complexity, nesting, class size, debt,
  detach, duplicates, hygiene, format, secret scan, obfuscation).
- Gating live-model prompt behavior; that remains a manual before/after
  benchmark (`BENCHMARK.md`). #576 measures harness overhead only.
- Runtime-loadable plugins, `dlopen`, or the post-1.0 core split (#563).

## 9. Decision log

- 2026-11-11: audit requested after the four incidents; findings accepted.
- 2026-11-12: ten child issues + epic created in the `post-1.0` milestone;
  P0 items labelled `urgency:now`.
- 2026-11-12: SDLC batching rule adopted: PR only at feature/bug completion,
  manual E2E recorded before opening; review mechanism tracked in #578.
- 2026-11-12: #569 CI watchdog implemented: `.github/workflows/ci-watchdog.yml`
  + `tools/ci_watchdog.py`, with fixture tests in `tests/gates/`.

# Spec: Harness Benchmark (Category 2 — the engine health report)

Status: **proposal — approved for implementation (BENCH-08)**
Companion: `benchmark/MISSION.md` (why), `benchmark/kpi-catalog.md` (model KPI catalog).

## Why this exists

The benchmark's one-line mission is: *"a diagnostic instrument for the
harness"* — the score of a model is a by-product. The `model` category
measures capability; it is correct and stays. What is missing is an axis that
measures **the harness itself** — the engine's own correctness, independent of
any model.

We found this the hard way: harness faults kept masquerading as model
weakness. The 32B bare-JSON tool-call bug read as "model scored 596"; the
oracle-path bug read as "15 scenarios at 60"; the stale h-02 oracle read as
"model failed 0/3". In every case the model KPI hid a harness fault, and one
bug contaminated 15–43 scenarios. The model benchmark cannot separate the two
by construction: it varies the model and holds the harness constant, so a
harness defect looks like a model regression.

Contract #3 of the mission is explicit about the fix: *"every hermetic failure
is the harness's failure."* This spec turns that principle into a first-class
run category.

## Principle

**A harness probe fixes both inputs and expectations software — the engine is
the only variable.** Each probe feeds a deterministic input (canned SSE bytes,
canned text, scripted tool calls, a context op sequence, a workspace layout)
into the engine and asserts the exact output. A deviation can *only* be a
harness bug. One bug cannot silently contaminate 15 scenarios; it fails the
probe that names it.

## Two categories

`amber-bench run` gains a category selector:

```
amber-bench run --cat model|harness|both
```

- `--cat model` (default) — everything today, unchanged: live + hermetic
  scenario suites, template engine, model_score progression. Purpose:
  identify the strongest model for baseline selection.
- `--cat harness` — deterministic probes only, no network, no live model,
  CI-safe. Purpose: the improvement cycle's internal KPI.
- default (no `--cat`) runs **both**; the report renders a two-axis card.
- `amber-bench report` shows `model_score` and `harness_integrity` side by
  side; a harness regression is flagged independently of any model delta.

A harness probe failure is **never** folded into model_score. The two axes
stay separate; a harness fault never masquerades as a model weakness, and vice
versa.

## Probe model

A probe is a small deterministic check: input fixture(s) → run engine →
assert. Probes are grouped into families by mechanism. Each family is one row
of the harness scorecard.

| Probe family | Mechanism isolated | Input | Assertion |
|---|---|---|---|
| `parse` | SSE delta → message/tool_calls reconstruction | canned SSE byte stream (tool_calls, content, reasoning, partial lines, `[DONE]`) | exact message fields + chunk sequence |
| `extract` | text → tool_calls extraction | content with bare-JSON, `<tool_call>` JSON, `<tools>`, attribute-style, mixed prose | exact call count, names, args |
| `dispatch` | tool_calls → registry → result round-trip | scripted tool calls, stub registry | each call executed once, correct tool, result sealed in context |
| `context` | Context push/pop/clear + FNV chain | scripted message sequence + a `const_cast` mutation attempt | `get_all()` asserts chain integrity; mutation path detected |
| `recovery` | retryable/non-retryable errors, mid-stream dropout, empty reply | FakeClient scripted errors | retries fire correctly; non-retryable does not loop forever; run completes |
| `envelope` | tool-status envelope parse | tool result envelopes (ok/err/timeout/denied, meta) | status/error/meta fields recovered |
| `budget` | max_steps/max_wall enforcement | scripted long loop against a small budget | loop ends at budget, `hard_stop` honest, no runaway |
| `confinement` | read/write/`../`/absolute-path confinement | workspace layout + escape attempts | escapes rejected, legal paths allowed |
| `oracle` | scenario self-validation | oracle JSON from the scenario corpus | every step's tool is one with a known arg schema; args keys match; exit without warnings |

Each probe emits:

```jsonc
{
  "family": "parse",
  "name": "parse_tool_calls_content_then_function",
  "pass": true,
  "detail": "3 calls, args round-tripped",      // what the engine produced
  "expected": "3 calls, args round-tripped",    // what was required
  "ms": 0.4
}
```

A failed probe's `detail`/`expected` pair is the weak-point report: it names
the mechanism and the deviation. That is the "where do we fix next?" answer.

## Harness scorecard

Per run:

- **per probe**: pass/fail + detail/expected deviation as above.
- **per family**: passed/total, e.g. `parse 5/5`, `budget 3/4`.
- **harness_integrity**: `passed_probes / total_probes` (0..1) — the headline
  engine-health number, reported alongside `model_score`.
- **run-over-run delta**: `harness report` compares against the previous run;
  a family that drops is a regression gate hit.

Probes are deterministic: same tree → same bit-for-bit result. A red probe is
a hard, local, actionable signal. This is the CI regression gate run on every
commit.

## Authoring rule (mission contract #2 & #6)

- A probe isolates **one mechanism**; if it cannot fail meaningfully it is
  removed.
- Every KPI in the harness scorecard has at least one probe that exercises it,
  or it is removed until it does.
- New probe = new RED test first: seed the fault (or, where the engine is
  currently correct, assert the property directly), see it fail, then fix.

## Files

- `bench/probe.h/.cpp` — probe registry, `ProbeResult`, family aggregation,
  harness scorecard struct. An `AgentHooks`/`FakeClient` style sibling to the
  recorder: probes reuse `tools/`, `lib/` engine functions as-is, no engine
  modification.
- `bench/main.cpp` — `--cat` selector, `harness` run path, two-axis report.
- `tests/bench_test.cpp` — probe RED/GREEN tests (hermetic, deterministic).
- Wire the harness probes into the CI regression gate alongside `make check`.

## Probe roadmap (the full failure surface)

The scorecard grows in planned increments (tracker: BENCH-09..11). Each
dimension below is one failure axis of the harness; a red probe names the
mechanism and the deviation.

| Dim | Families | What it isolates | Status |
|---|---|---|---|
| Mechanism fidelity | parse, extract, context, envelope, confinement, oracle | SSE/text/context/envelope/workspace mechanics | shipped (BENCH-08) |
| Dispatch & budget | dispatch, budget | call→registry→result round-trip; step budget | shipped (BENCH-08) |
| Recovery | recovery | retryable-error compensation | shipped (BENCH-08) |
| **Agentic loop** | loop | plan design + execution adherence (graph/chain); done-flag termination; continue-flag non-termination; infinite-loop breakout ≤3 repeats; text-loop steer @2 / hard stop @5; fail-streak steer+stop; breakout false-positive precision; hard-stop honesty; plan-adherence chain | BENCH-09 |
| **Tool-call fidelity** | fidelity | wrong-tool misuse; value-level arg precision; unknown tool → graceful error; malformed-args repair; object-vs-string wire shapes | BENCH-10 |
| **Output interpretation** | output | action-after-read (next call reflects tool output); 64 KiB truncation; envelope error-text + meta round-trip | BENCH-11 |
| **Telemetry persistence** | (report) | per-call `{name, args, ok, error, denied, timeout, duration_ms}` in results JSON; calls_per_step histogram; failure taxonomy | BENCH-11 |

### New KPIs (with the agentic loop)

- `adherence_ratio` — executed tool sequence satisfies the plan's dependency
  edges / total edges (the graph match).
- `breakout_latency` — turns until loop detection fires (≤3 for tool loops).
- `steer_effectiveness` — runs completing after a recovery steer / runs that
  received a steer.
- `detector_precision` — legit distinct repeats NOT flagged (false-positive
  rate of the loop detector).
- `calls_per_step` p50/p95/max — detects call bursting vs steady pacing.
- `failure_taxonomy` — tool_failures split by error/timeout/denied/
  unknown-tool/malformed-args.

These are the "winning / losing / stagnating" signals: each is monotonic and
trends across runs, unlike a single composite score.

## Non-goals

- No engine modification: probes exercise `lib/` + `tools/` over their public
  APIs. A probe that needs a signal only visible inside a loop body is a
  finding, not a reason to add in-loop telemetry (that is a separate, spec'd
  engine change).
- The `model` category is unchanged; this category does not re-rank models.
- No live-model harness runs: live variance would break determinism, so the
  harness axis is hermetic by construction. Live model runs keep using the
  `model` category.
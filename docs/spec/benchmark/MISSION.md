# Benchmark Mission

**How the benchmark achieves the vision. The shield.**

The vision says *what* we build. The benchmark says *whether what we build
actually works* — objectively, repeatably, and continuously. This document is
the reference and baseline for the benchmark itself: why it exists, what it
measures, what "strong" means, and how we use it to keep the harness from
degrading.

Companion specs: `benchmark/kpi-framework.md` (harness architecture),
`benchmark/kpi-catalog.md` (measurable indicators), `benchmark/corpus.md`
(scenario taxonomy).

---

## The one-line mission

**The benchmark is a diagnostic instrument for the harness, not a leaderboard.
Every scenario is an assertion about how the agent runs; every failure is an
objective weak-point report that tells us what to fix next.**

The score of a model is a *by-product*. The product is understanding.

---

## Why we exist (the dark)

Without strong KPIs we walk in the dark:

- we do not know what to improve,
- we do not know why anything regressed,
- we have no way of proving progress,
- we can only follow the slippery slope of destructive degradation —
  each change looks fine in isolation, and nothing catches the slow
  accumulation of breakage.

The benchmark is the shield against that. Measured, repeatable, honest — it
turns the harness's behavior into numbers we can interrogate, so every
engineering decision is a response to evidence, not a guess.

---

## The core contract

These principles are non-negotiable. Any change to the harness or the
benchmark must preserve them.

### 1. KPIs over vibes

A claim about the harness ("recovery works", "tool selection is efficient",
"the loop is stable") is only true when a scenario asserts it and the KPI
proves it. If a behavior cannot be measured, it is not finished.

### 2. Purposeful tests, not point generators

Every scenario isolates **one mechanism** and asserts its behavior. A
scenario that cannot fail meaningfully — one that saturates for every model,
or whose failure is format noise rather than substance — is removed or
reworked. We do not accumulate points; we accumulate diagnoses.

### 3. Deterministic hermetic suite = harness truth

The fake-LLM suite runs the engine with a scripted model: deterministic,
network-free, CI-safe. Model-independent by construction — **every hermetic
failure is the harness's failure**. This is the regression gate: the harness
cannot silently degrade without a red run.

### 4. Live suite = model capability on a proven harness

A live model's result only means something because the harness underneath it
is verified. Strong models exercise the most paths; **the stronger the model,
the more its failures implicate the harness**. Weak models, in turn, expose
recovery gaps. Both are findings — the live suite is where the model library
talks back.

### 5. Partial credit, honest failures

Scoring is continuous and discriminating (see `kpi-catalog.md` for the exact
weights): bullseye plus argument fidelity, tool economy, efficiency,
robustness, adherence. A failed scenario keeps its honest partial credit
capped at 60 — pass counts and scores must never diverge. A sub-score that
saturates for everyone is dead weight and gets reworked.

### 6. Every KPI is a hypothesis, tested

The KPI catalog is not a wishlist: each indicator must have at least one
scenario that exercises it, or it is removed until it does.

---

## What we measure (the dimensions map)

The corpus is organized along the dimensions that matter for the harness and
for model capability. Each dimension is a suite; each suite is a row in the
per-model matrix.

| Dimension | What it diagnoses | Primary instrument |
|---|---|---|
| **Agentic loop** | Loop consistency across turns, iteration caps, text-loop and duplicate-call detection | Hermetic |
| **Tool execution** | Argument round-trip, dispatch order, malformed-schema repair, tool death mid-call | Hermetic |
| **Failure recovery** | Retries, backoff, mid-stream dropout, empty replies, denied tools, server 4xx adaptation | Hermetic |
| **Tool overload** | Many calls per message, huge outputs, context overflow, iteration pressure | Hermetic |
| **Output understanding** | Structured tool output parsed and acted on correctly | Hermetic + live |
| **Tool choice** | Choosing the right tool for the job (search vs grep vs bash, read vs cat, ...) | Live |
| **Repeats & misuse** | Redundant calls, forbidden tools, wasted work | Hermetic + live |
| **Terminal proficiency** | Bash semantics: pipes, background jobs, timeouts, cwd, exit codes, caps | Live |
| **Software development** | Real implementation/refactor quality against hidden tests (template engine) | Live |
| **Context dilution** | Logic following across long conversations and large contexts | Live |
| **Quality perception** | Articulated judgment: architecture, refactoring, quality, data structures, patterns, clean code (review suite) | Live |

A model's per-suite matrix is its capability profile; the same matrix over the
hermetic suite is the harness's health report.

---

## The model library strategy

We do not only chase the strongest model. The library spans every tier, and
each tier has a job:

1. **Strong models** exercise the most paths first. Their failures are almost
   always harness bugs — fix the harness until the strong model's runs are
   clean.
2. **Medium models** confirm the harness works on a broader population.
3. **Weak models** expose recovery gaps — the harness must compensate with
   steer messages, tool-selection hints, argument repair, and graceful
   degradation. Building recovery for the weakest tier is how the harness
   becomes a solid tool for everyone.

The fix cycle is therefore **strongest-first, cascade down**: fix the harness
on the strongest model, then keep the fixes honest by re-running the weaker
tiers and adding the recovery actions they reveal.

`model_robustness` (same model under good/poor profiles) is part of every
tier's evaluation — stability is a quality signal.

---

## Definitions of done (what "strong" means)

The benchmark is strong when all of these hold:

- [ ] The hermetic suite is fully green in CI — the regression gate runs on
      every commit and a harness regression is a red build.
- [ ] Every dimension above has enough purposeful scenarios to produce a
      stable per-suite signal (no dimension below 5 scenarios at full scope).
- [ ] No sub-score is saturated: each one separates models or runs, or it is
      reworked.
- [ ] Baselines are reproducible: comparisons use medians over repeats, not
      single runs; committed results are schema-stable and regenerable.
- [ ] Every harness fix is preceded by a failing scenario (red) and proven by
      its green run — the fix log is the benchmark's changelog.
- [ ] The documentation (this mission, the spec, the catalog, the corpus)
      matches the implementation; drift is a defect.

---

## The roadmap

| Phase | Scope |
|---|---|
| **0 — Baselines** | Run the model library under scoring v2; every failure is a candidate harness finding |
| **1 — Hermetic diagnostic suite** | The mechanism isolators: loop consistency, text-loop and duplicate detection, tool-call explosion, iteration caps, malformed-args repair, mid-turn tool death, server 4xx recovery, large-output overflow, structured-output parsing |
| **2 — Live capability suites** | Terminal proficiency (bash semantics), software development volume (multi-file templates), context dilution suite (long-conversation logic) |
| **3 — Model library operations** | Tier classification, per-suite capability matrix, model_robustness (good/poor), the strongest-first cascade with recovery actions for weaker tiers |
| **4 — Methodology** | `--repeat` medians/p95, the regression gate with trend history and delta alerts |

---

## What the benchmark is NOT

- **Not a leaderboard** — rankings are a by-product, never the goal; a number
  without a diagnosis is noise.
- **Not a point generator** — scenarios that cannot diagnose are removed.
- **Not a substitute for authoring judgment** — a scenario is only as honest
  as its oracle and its checks; authoring quality is where the benchmark
  lives or dies.
- **Not a one-time exercise** — the benchmark is a living instrument: it
  grows with the harness, and the harness is only as trustworthy as the
  benchmark that watches it.

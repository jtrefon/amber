# Benchmark

**What this is**: a KPI record of the *amber harness* — how well the agent
loop, tool schemas and prompts drive each model to the correct solution. This
is a harness score, not a model leaderboard: every scenario runs through the
same engine, and the score measures the harness+model pair over 31 scenarios
across 6 suites (agent failures, terminal work, tool selection, prompt
adherence, coding, refactoring).

**Model loading (single GPU — one model at a time).** The machine has one
GPU; never run two servers, never test with reasoning off (MoE models need
reasoning to engage experts — `models.ini` presets all ship
`REASONING=on`). Small models swap in-process via
`POST /models/load {"model": "<preset>"}`; large ones (e.g. gemma4-31b)
OOM during the swap, so load them by restarting the service with the model
as the CLI arg on the same port. The tracked runs below were produced with
reasoning enabled for every model.

**How to reproduce** (requires a running OpenAI-compatible server, e.g.
llama.cpp / llama-turboq with the presets below):

```sh
./amber-bench run --live --profile qwopus-27b   --out .amber/bench/results/qwopus-27b.json
./amber-bench run --live --profile gemma4-12b-q4 --out .amber/bench/results/gemma4-12b-q4.json
./amber-bench run --live --profile gemma4-31b   --out .amber/bench/results/gemma4-31b.json
./amber-bench run --live --profile qwen35-dense   --out .amber/bench/results/qwen35-dense.json
./amber-bench run --live --profile qwen36-27b-mtp --out .amber/bench/results/qwen36-27b-mtp.json
./amber-bench run --live --profile qwen35-moe     --out .amber/bench/results/qwen35-moe.json
./amber-bench run --live --profile ornith-35b    --out .amber/bench/results/ornith-35b.json
./amber-bench run --live --profile kilo-auto-free --out .amber/bench/results/kilo-auto-free.json   # needs AMBER_API_KEY (kilo.ai gateway)
./amber-bench run --live --profile nemotron-550b  --out .amber/bench/results/nemotron-550b.json    # needs AMBER_API_KEY
./amber-bench report bench/results/*.json --format markdown
```

Scoring: each scenario earns 0-100 from four sub-scores — correctness
(bullseye + hidden-test artifact), efficiency (steps/wasted/redundant vs
expected), robustness (retries/recoveries/hard-stops) and adherence
(prompt-checks minus forbidden-tool use). The model score is the
difficulty-weighted aggregate (1-5 per scenario), scaled to /1000. Partial
credit everywhere: a weak model loses points per missed oracle step, per
wasted call, per failed hidden test.

Raw per-run JSON lives in `bench/results/`. The baseline (2026-08-03) is
tracked here so we can measure our own rate of improvement as the engine
changes — rerun the same commands after any harness change and compare.

> Naming note: `qwen35-dense` is the preset for **Qwen3.6-27B dense**
> (27B, not MoE, not Qwen3.5 — the 35B mixture-of-experts is
> `Qwen3.6-35B-A3B-UD`, reported as "Qwen3.6-35B MoE (A3B)"). All three
> 27B Qwen-family runs (qwopus-27b, Qwen3.6-27B dense, Qwen3.6-27B MTP)
> land in a ~900-910 band — differences within it are single-run variance,
> not model ranking; use `--repeat N` for statistically meaningful deltas.

## Official benchmark cross-check (the 27B vs 550B question)

Why does a 27B dense model outscore a 550B frontier model on this harness?
It is not a harness artifact — the official numbers say the same thing:

| Model | SWE-bench Verified | Terminal-Bench |
|---|---|---|
| Qwen3.6-27B (dense, Apr 2026) | **77.2%** | **59.3%** (TB 2.0) |
| NVIDIA Nemotron 3 Ultra 550B-A55B (Jun 2026) | 71.9% | 56.4% (TB 2.1) |

Sources: Qwen first-party model card (77.2 / 59.3, purpose-built for
agentic coding and repository-level reasoning — it also beats the 397B
MoE flagship); NVIDIA / evals.report verified scores for Nemotron 3 Ultra
(SWE-bench Verified 71.9, Terminal-Bench 2.1 56.4, GPQA Diamond 87.0).

**Conclusion**: the harness ranking direction is validated by the official
benchmarks — this particular 27B dense model is genuinely the better
*agentic coding* model. Nemotron's strengths (GPQA 87%, 1M context,
long-context analysis, math/science) live on axes this corpus barely
touches. The harness gap (910 vs 806) is wider than the official gap
(77.2 vs 71.9), because the corpus is small single-file tasks where tool
economy and termination dominate — exactly the axis where the 27B's coding
training wins. To measure the 550B's actual strengths, the corpus needs a
repo-level suite: multi-file bugs, long-context tasks, deeper reasoning.

## P5: compression gate fixes (2026-08-04)

Two gate defects found and fixed (TDD):

1. **Budget cap**: the gate fired at 50% of auto-detected `n_ctx`
   (~131k tokens with the 262k local server) — effectively never during a
   run. The effective budget is now capped at 32k regardless of n_ctx.
2. **Cooldown-from-zero bug (the true root cause)**: `last_compress_turn_`
   starts at 0, and the cooldown check `(turn - 0) < 20` blocked the FIRST
   compression of every session until turn 20 — compression effectively
   never ran in normal use. Fixed: cooldown applies only after the first
   compression (turn-0 loaded-session protection retained).

Proof: unit tests (gate fires at 16k with 262k n_ctx; first-compress not
cooldown-blocked) + new hermetic scenario **k-01-compression-stress**
(scripted 10+ message run crosses the gate mid-task → compression fires
(`compressions: 1` KPI) → final answer correct, PASS). Live corpus still
cannot trigger it: our scenarios stay under ~7k tokens vs the 16k firing
point (the same horizon finding as P1/P4). New `compressions` KPI in the
recorder/report.

## Repo-level suite (long-horizon, 2026-08-04)

Two new repo scenarios with real verification (hidden tests compile and run
the project): r-01 cross-file interface rename (search → edit all call
sites → compile), r-02 seeded bug-hunt (explore → fix → verify).

| Scenario | Qwen3.6-27B dense | todowrite used |
|---|---|---|
| r-01-rename-interface | PASS 100 (7 steps, artifact 1) | 0 |
| r-02-bug-hunt | PASS 96 (artifact 1) | 0 |

Advertisement experiment (same day): tools.md + schema description
rewritten with concrete triggers ("a task list earns its keep when the work
has shape: three or more distinct steps, steps that depend on earlier ones,
files that change together, or new instructions arriving mid-task"). Wire
capture confirms the model received the trigger-rich description verbatim
on every request. **Adoption: still 0** on both repo scenarios (both still
PASS: 100 / 96).

**Complete verdict across the hypothesis chain** — all three disproven with
wire-level evidence: (1) not wiring — advertised in every request; (2) not
task complexity — long-horizon tasks solved perfectly without the tool;
(3) not advertisement — trigger-rich text delivered verbatim, still unused.
The residual explanation is **model training fit (H5)**: Qwen3.6-27B's RL
traces do not include the TodoWrite convention that Claude/GPT-class models
are trained with. The tool remains functional, advertised, and harmless —
it becomes relevant with sub-agent delegation (P4) and models trained on
the convention.

Verdict on the "not complex enough" hypothesis: **complexity is not the
trigger.** The baseline model completes multi-phase, dependent-step work
efficiently without any planning tool (r-01 at 100, surgical edits, zero
redundancy). The horizon where externalized task tracking pays (context
degradation on very long runs) is beyond this corpus. The todowrite tool
remains functional, advertised (wire-verified), and harmless — it becomes
relevant with P4 sub-agents and much longer scenarios.

## P1: todowrite tool — externalized task tracking (2026-08-04)

`todowrite` tool shipped (host-owned `TodoStore`, full-list replacement —
the TodoWrite contract strong agentic models are trained on; survives
compression by design). Hermetic tests prove state persists across turns.

| Model | before (2 runs) | after (3 runs) | verdict |
|---|---|---|---|
| Qwen3.6-27B dense | 885, 913 — mean 899 | 911, 895, 908 — mean 905 | **neutral** (+6 ≈ noise) |

Per-scenario (25 shared scenarios, before-r2 vs mean-of-3-after): 2 improved,
19 stable, 4 regressed — the regressions (c-03 -21, c-02 -4, c-04 -4) are
single-run variance swings already observed in earlier runs (c-03 ranged
67-88 across all runs). Agentic 69→66, plan% 65→63, redundant 9→7-12: all
within the established variance band.

**Verdict: no degradation, no measurable improvement, zero adoption**

Adoption proof (wire-level): `--debug` capture of a live t-07 run shows all
11 tools — including `todowrite` — advertised in every request the model
received; the model saw the tool and declined it on all 5 requests. The tool
executed correctly in hermetic tests (state persists across turns). Zero
adoption is genuine model behavior on single-pass tasks, not a wiring bug. — the current corpus is
single-pass work where planning cannot pay (opencode's own guidance: skip
when the task is straightforward). Proof of value is gated on the
repo-level suite (long-horizon tasks), which is also the gate for the
sub-agent work. The tool is prerequisite infrastructure, not a lever on
this corpus.

## Prompt v2 (empowerment) — before/after

`prompts/system.md` was rewritten from confinement language ("never fabricate",
"each turn must advance", "dont stop unless stuck") to a descriptive,
empowerment-style prompt: role, personality, working style, response
framework, closing convention. Same scenarios, same models, same harness —
the only change is the prompt. Raw records: `bench/results/prompt-v2/`.

| Model | Before | After | Verdict |
|---|---|---|---|
| Nemotron 550B (free) | 806 / 20-25 | 821 / 21-25 | **t-01-search audit-loop termination fixed** |
| Qwen3.6-27B dense | 910 / 24-25 | 885, 913 / 24-25 | within variance, no regression |
| qwopus-27b | 900 / 23-25 | 906 / 24-25 | +6, earlier t-03 failure cleared |

The prompt philosophy (descriptive over prohibitive) is now a repo
convention: prompts describe role, personality, environment and tooling;
they empower rather than confine. No forcing or forbidding language.

### Free-tier challengers (prompt v2)

| Model | score | pass | agentic | plan% | tools | redundant | Official (SWE-bench / TB) |
|---|---|---|---|---|---|---|---|
| Qwen3.6-27B dense (local) | 913 | 24/25 | 71 | 69 | 104 | 9 | 77.2 / 59.3 |
| qwopus-27b (local) | 906 | 24/25 | 66 | 62 | 105 | 13 | — / — |
| **Laguna S 2.1 (free)** | **887** | 23/25 | 66 | 60 | 117 | **4** | 59.4 Pro / **70.2** |
| Nemotron 550B (free) | 821 | 21/25 | 54 | 54 | 142 | 17 | 71.9 / 56.4 |
| Hy3 (free, untested here) | — | — | — | — | — | — | 78.0 / 54.4 |

Laguna S 2.1 is the strongest free challenger: lowest redundancy of any
model tested (4 identical re-calls) and the best official Terminal-Bench
score (70.2, +11 over Qwen) — its weakness on this corpus is the
multi-constraint format scenario (p-05) and higher tool count on small
tasks. No free Kilo model significantly beats the Qwen 27B dense on this
harness; the models that do (Opus-class, GLM-5 77.8, Kimi K2.5 76.8) are
paid tiers.

> Cloud runs (kilo-auto/free, Nemotron 550B) go through the Kilo AI Gateway
> (`https://api.kilo.ai/api/gateway`, provider preset `kilocode`); the free
> tier auto-routes `kilo-auto/free` to the best available free model
> (a small flash on the recorded run), while the Nemotron run targets the
> explicit 550B free endpoint. Cloud runs carry rate-limit and routing
> variance.

> Reasoning note: all runs ship with reasoning enabled. Records predating
> explicit reasoning tracking (qwopus-27b, qwen35-dense, ornith-35b,
> gemma4-12b-q4) are labelled `on (server preset, client auto)` — the
> server preset carried `REASONING=on` and the client sent no override.
> `gemma4-12b-q4-reasoning` is the same model re-run with the client
> explicitly requesting reasoning (`enable_thinking=true`) for comparison:
> the ~74-point delta is mostly fixed oracle false-failures plus the
> model's high run-to-run variance (it flips which templates it solves),
> not a reasoning flip.
# Benchmark: harness score by model

- **qwopus-27b**: 900/1000
- **Qwen3.6-27B dense**: 910/1000
- **Qwen3.6-27B MTP**: 901/1000
- **Qwen3.6-35B MoE (A3B)**: 877/1000
- **ornith-1.0-35b**: 837/1000
- **gemma4-12b-q4**: 726/1000
- **gemma4-12b-q4 (reasoning explicit)**: 799/1000
- **gemma4-31b**: 903/1000
- **kilo-auto/free**: 861/1000
- **NVIDIA Nemotron 3 Ultra 550B (free)**: 806/1000

| model | score | agentic | plan % | tools | fail % | redun % | steps | wall (s) |
|---|---|---|---|---|---|---|---|---|
| qwopus-27b | 900 | 62 | 44 | 109 | 4 | 15 | 121 | 321.902 |
| Qwen3.6-27B dense | 910 | 68 | 46 | 104 | 3 | 9 | 115 | 781.901 |
| Qwen3.6-27B MTP | 901 | 66 | 47 | 104 | 1 | 11 | 117 | 308.714 |
| Qwen3.6-35B MoE (A3B) | 877 | 59 | 35 | 137 | 8 | 16 | 145 | 548.405 |
| ornith-1.0-35b | 837 | 63 | 39 | 124 | 7 | 25 | 153 | 497.099 |
| gemma4-12b-q4 | 726 | 70 | 47 | 103 | 7 | 11 | 127 | 414.075 |
| gemma4-12b-q4 (reasoning explicit) | 799 | 66 | 43 | 111 | 12 | 16 | 135 | 580.849 |
| gemma4-31b | 903 | 72 | 47 | 103 | 1 | 4 | 127 | 1056.49 |
| kilo-auto/free | 861 | 63 | 42 | 115 | 6 | 6 | 121 | 364.541 |
| NVIDIA Nemotron 3 Ultra 550B (free) | 806 | 58 | 36 | 133 | 6 | 15 | 149 | 1087.47 |

| scenario | qwopus-27b | Qwen3.6-27B dense | Qwen3.6-27B MTP | Qwen3.6-35B MoE (A3B) | ornith-1.0-35b | gemma4-12b-q4 | gemma4-12b-q4 (reasoning explicit) | gemma4-31b | kilo-auto/free | NVIDIA Nemotron 3 Ultra 550B (free) |
|---|---|---|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 96 | 96 | 96 | 100 | 96 | 84 | 86 | 100 | 100 | 88 |
| c-02-sorting-multi | 96 | 96 | 100 | 40 | 40 | 95 | 40 | 90 | 100 | 82 |
| c-03-ring-buffer | 84 | 80 | 80 | 80 | 84 | 86 | 34 | 80 | 80 | 40 |
| c-04-lcs | 80 | 88 | 84 | 80 | 84 | 90 | 90 | 84 | 84 | 80 |
| c-05-graph-bfs | 84 | 86 | 80 | 82 | 82 | 90 | 90 | 84 | 82 | 80 |
| p-01-envelope-format | 100 | 100 | 100 | 100 | 96 | 100 | 96 | 96 | 96 | 100 |
| p-02-banned-tool-refusal | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| p-03-verify-after-action | 15 | 27 | 21 | 25 | 21 | 21 | 19 | 29 | 25 | 29 |
| p-05-multi-constraint | 100 | 100 | 100 | 100 | 100 | 48 | 48 | 100 | 48 | 48 |
| p-06-verify-before-claim | 98 | 94 | 94 | 98 | 94 | 98 | 94 | 94 | 94 | 98 |
| r-01-extract-method | 96 | 94 | 96 | 92 | 96 | 40 | 100 | 100 | 96 | 96 |
| r-02-polymorphism-over-switch | 92 | 86 | 96 | 80 | 96 | 89 | 81 | 100 | 88 | 94 |
| r-03-adapter | 80 | 82 | 80 | 86 | 80 | 27 | 80 | 80 | 82 | 80 |
| r-04-strategy | 88 | 88 | 86 | 84 | 80 | 34 | 90 | 82 | 82 | 82 |
| t-01-ls-count-files | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| t-02-grep-extract-value | 100 | 98 | 100 | 100 | 98 | 100 | 100 | 100 | 100 | 100 |
| t-03-compile-run-cpp | 100 | 100 | 100 | 100 | 46 | 100 | 100 | 100 | 46 | 46 |
| t-04-env-inventory | 100 | 100 | 100 | 100 | 100 | 44 | 100 | 100 | 100 | 100 |
| t-05-pipeline-transform | 100 | 100 | 100 | 98 | 100 | 46 | 98 | 100 | 100 | 100 |
| t-06-multi-dir-count | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| t-01-search-vs-bash-grep | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 30 |
| t-02-read-vs-cat | 100 | 100 | 100 | 100 | 94 | 100 | 92 | 100 | 100 | 100 |
| t-03-write-vs-rewrite | 92 | 100 | 92 | 100 | 35 | 92 | 84 | 92 | 92 | 92 |
| t-04-process-vs-blocking | 80 | 90 | 82 | 80 | 80 | 32 | 26 | 86 | 90 | 80 |
| t-06-write-verify-loop | 90 | 90 | 90 | 90 | 90 | 82 | 90 | 82 | 90 | 90 |

---

## qwopus-27b

- run: `run-1785784927270481946` [live, engine 0.3.1, reasoning on]
- **model score: 900/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 96 | 1 | 7 | 6 | 11.865 | 1 |
| c-02-sorting-multi | 3 | 96 | 1 | 8 | 7 | 23.839 | 1 |
| c-03-ring-buffer | 4 | 84 | 1 | 7 | 7 | 21.58 | 1 |
| c-04-lcs | 4 | 80 | 1 | 12 | 11 | 34.261 | 1 |
| c-05-graph-bfs | 5 | 84 | 1 | 7 | 6 | 25.872 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 6.219 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 4.767 | - |
| p-03-verify-after-action | 3 | 15 | 0 | 8 | 7 | 24.386 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 5.725 | - |
| p-06-verify-before-claim | 4 | 98 | 1 | 2 | 0 | 4.616 | - |
| r-01-extract-method | 4 | 96 | 1 | 6 | 5 | 20.961 | 1 |
| r-02-polymorphism-over-switch | 4 | 92 | 1 | 8 | 7 | 21.831 | 1 |
| r-03-adapter | 4 | 80 | 1 | 11 | 13 | 22.287 | 1 |
| r-04-strategy | 5 | 88 | 1 | 5 | 5 | 16.106 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 4.887 | - |
| t-02-grep-extract-value | 3 | 100 | 1 | 2 | 0 | 4.913 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 7.103 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 4.482 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 3 | 0 | 7.952 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 5.122 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 4.701 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 4.922 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 6.379 | - |
| t-04-process-vs-blocking | 5 | 80 | 1 | 8 | 4 | 19.975 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 7.151 | - |

### Failures

- **p-03-verify-after-action** (15/100): oracle not matched: 0/2 steps (bullseye 0)

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 109 | 4.36 |
| tool failures | 5 | 0.2 |
| tool denials | 0 | 0 |
| redundant calls | 17 | 0.68 |
| LLM retries | 0 | 0 |
| wall time (s) | 321.902 | 12.8761 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 109 |
| total deviation (extra calls) | 60 |
| plan efficiency | 44/100 |
| agentic score (mean plan adherence) | 62/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 39 | 24 | 38 |
| process_read | 1 | 3 | 2 | 33 |
| process_start | 1 | 1 | 0 | 100 |
| read | 18 | 49 | 31 | 36 |
| search | 1 | 1 | 0 | 100 |
| write | 12 | 15 | 3 | 80 |

## Qwen3.6-27B dense

- run: `run-1785781966685651020` [live, engine 0.3.1, reasoning on]
- **model score: 910/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 96 | 1 | 6 | 5 | 42.886 | 1 |
| c-02-sorting-multi | 3 | 96 | 1 | 10 | 9 | 115.682 | 1 |
| c-03-ring-buffer | 4 | 80 | 1 | 12 | 10 | 90.43 | 1 |
| c-04-lcs | 4 | 88 | 1 | 7 | 6 | 46.256 | 1 |
| c-05-graph-bfs | 5 | 86 | 1 | 8 | 7 | 56.222 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 9.778 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 9.54 | - |
| p-03-verify-after-action | 3 | 27 | 0 | 3 | 2 | 13.342 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 10.24 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 13.363 | - |
| r-01-extract-method | 4 | 94 | 1 | 9 | 9 | 73.926 | 1 |
| r-02-polymorphism-over-switch | 4 | 86 | 1 | 9 | 9 | 66.026 | 1 |
| r-03-adapter | 4 | 82 | 1 | 8 | 10 | 56.385 | 1 |
| r-04-strategy | 5 | 88 | 1 | 5 | 5 | 39.168 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 9.557 | - |
| t-02-grep-extract-value | 3 | 98 | 1 | 2 | 0 | 9.913 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 18.715 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 6.923 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 2 | 0 | 12.579 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 9.83 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 9.747 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 10.93 | - |
| t-03-write-vs-rewrite | 3 | 100 | 1 | 3 | 0 | 13.193 | - |
| t-04-process-vs-blocking | 5 | 90 | 1 | 5 | 1 | 20.171 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 17.099 | - |

### Failures

- **p-03-verify-after-action** (27/100): oracle not matched: 0/2 steps (bullseye 0)

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 104 | 4.16 |
| tool failures | 4 | 0.16 |
| tool denials | 0 | 0 |
| redundant calls | 10 | 0.4 |
| LLM retries | 0 | 0 |
| wall time (s) | 781.901 | 31.276 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 104 |
| total deviation (extra calls) | 55 |
| plan efficiency | 46/100 |
| agentic score (mean plan adherence) | 68/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 36 | 21 | 41 |
| process_read | 1 | 2 | 1 | 50 |
| process_start | 1 | 1 | 0 | 100 |
| read | 18 | 48 | 30 | 37 |
| search | 1 | 2 | 1 | 50 |
| write | 12 | 14 | 2 | 85 |

## Qwen3.6-27B MTP

- run: `run-1785784252052460619` [live, engine 0.3.1, reasoning on]
- **model score: 901/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 96 | 1 | 8 | 7 | 16.692 | 1 |
| c-02-sorting-multi | 3 | 100 | 1 | 7 | 6 | 24.835 | 1 |
| c-03-ring-buffer | 4 | 80 | 1 | 9 | 9 | 33.503 | 1 |
| c-04-lcs | 4 | 84 | 1 | 7 | 5 | 18.67 | 1 |
| c-05-graph-bfs | 5 | 80 | 1 | 10 | 8 | 22.602 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 5.173 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 4.578 | - |
| p-03-verify-after-action | 3 | 21 | 0 | 4 | 3 | 8.371 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 5.542 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 6.209 | - |
| r-01-extract-method | 4 | 96 | 1 | 7 | 7 | 23.245 | 1 |
| r-02-polymorphism-over-switch | 4 | 96 | 1 | 8 | 8 | 24.827 | 1 |
| r-03-adapter | 4 | 80 | 1 | 9 | 10 | 22.016 | 1 |
| r-04-strategy | 5 | 86 | 1 | 6 | 5 | 19.396 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 5.187 | - |
| t-02-grep-extract-value | 3 | 100 | 1 | 2 | 0 | 4.762 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 7.549 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 5.298 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 2 | 0 | 7.39 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 3 | 0 | 6.193 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 5.33 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 5.971 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 7.383 | - |
| t-04-process-vs-blocking | 5 | 82 | 1 | 7 | 3 | 11.091 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 6.901 | - |

### Failures

- **p-03-verify-after-action** (21/100): oracle not matched: 0/2 steps (bullseye 0)

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 104 | 4.16 |
| tool failures | 2 | 0.08 |
| tool denials | 0 | 0 |
| redundant calls | 12 | 0.48 |
| LLM retries | 0 | 0 |
| wall time (s) | 308.714 | 12.3486 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 104 |
| total deviation (extra calls) | 54 |
| plan efficiency | 46/100 |
| agentic score (mean plan adherence) | 66/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 31 | 16 | 48 |
| process_read | 1 | 2 | 1 | 50 |
| process_start | 1 | 1 | 0 | 100 |
| read | 18 | 52 | 34 | 34 |
| search | 1 | 1 | 0 | 100 |
| write | 12 | 15 | 3 | 80 |

## Qwen3.6-35B MoE (A3B)

- run: `run-1785792736852620357` [live, engine 0.3.1, reasoning on]
- **model score: 877/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 100 | 1 | 7 | 7 | 14.676 | 1 |
| c-02-sorting-multi | 3 | 40 | 1 | 20 | 20 | 171.527 | 1 |
| c-03-ring-buffer | 4 | 80 | 1 | 10 | 10 | 39.953 | 1 |
| c-04-lcs | 4 | 80 | 1 | 12 | 11 | 28.35 | 1 |
| c-05-graph-bfs | 5 | 82 | 1 | 10 | 9 | 25.186 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 3.005 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 3.195 | - |
| p-03-verify-after-action | 3 | 25 | 0 | 4 | 3 | 5.546 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 3.256 | - |
| p-06-verify-before-claim | 4 | 98 | 1 | 2 | 0 | 2.719 | - |
| r-01-extract-method | 4 | 92 | 1 | 7 | 8 | 28.511 | 1 |
| r-02-polymorphism-over-switch | 4 | 80 | 1 | 16 | 17 | 62.212 | 1 |
| r-03-adapter | 4 | 86 | 1 | 8 | 8 | 31.044 | 1 |
| r-04-strategy | 5 | 84 | 1 | 7 | 6 | 74.425 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 2.9 | - |
| t-02-grep-extract-value | 3 | 100 | 1 | 2 | 0 | 2.856 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 5.28 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 2.563 | - |
| t-05-pipeline-transform | 4 | 98 | 1 | 6 | 0 | 11.119 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 3.205 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 2.879 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 3.059 | - |
| t-03-write-vs-rewrite | 3 | 100 | 1 | 3 | 0 | 4.248 | - |
| t-04-process-vs-blocking | 5 | 80 | 1 | 8 | 4 | 10.954 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 5.737 | - |

### Failures

- **c-02-sorting-multi** (40/100): final answer failed scenario checks
- **p-03-verify-after-action** (25/100): oracle not matched: 0/2 steps (bullseye 0)

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 137 | 5.48 |
| tool failures | 12 | 0.48 |
| tool denials | 1 | 0.04 |
| redundant calls | 22 | 0.88 |
| LLM retries | 0 | 0 |
| wall time (s) | 548.405 | 21.9362 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 137 |
| total deviation (extra calls) | 88 |
| plan efficiency | 35/100 |
| agentic score (mean plan adherence) | 59/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 44 | 29 | 34 |
| process_read | 1 | 3 | 2 | 33 |
| process_start | 1 | 1 | 0 | 100 |
| read | 18 | 60 | 42 | 30 |
| search | 1 | 1 | 0 | 100 |
| write | 12 | 27 | 15 | 44 |

## ornith-1.0-35b

- run: `run-1785777586223645316` [live, engine 0.3.1, reasoning default]
- **model score: 837/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 96 | 1 | 7 | 6 | 16.242 | 1 |
| c-02-sorting-multi | 3 | 40 | 1 | 20 | 20 | 135.772 | 1 |
| c-03-ring-buffer | 4 | 84 | 1 | 7 | 6 | 23.121 | 1 |
| c-04-lcs | 4 | 84 | 1 | 7 | 6 | 15.622 | 1 |
| c-05-graph-bfs | 5 | 82 | 1 | 8 | 7 | 24.12 | 1 |
| p-01-envelope-format | 2 | 96 | 1 | 3 | 1 | 5.097 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 3.408 | - |
| p-03-verify-after-action | 3 | 21 | 0 | 4 | 3 | 7.216 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 4.073 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 4.889 | - |
| r-01-extract-method | 4 | 96 | 1 | 6 | 5 | 23.923 | 1 |
| r-02-polymorphism-over-switch | 4 | 96 | 1 | 8 | 7 | 28.272 | 1 |
| r-03-adapter | 4 | 80 | 1 | 16 | 7 | 50.043 | 1 |
| r-04-strategy | 5 | 80 | 1 | 20 | 19 | 83.871 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 3.49 | - |
| t-02-grep-extract-value | 3 | 98 | 1 | 2 | 0 | 3.276 | - |
| t-03-compile-run-cpp | 3 | 46 | 1 | 4 | 0 | 6.752 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 2.942 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 4 | 0 | 7.482 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 3.709 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 3.479 | - |
| t-02-read-vs-cat | 2 | 94 | 1 | 2 | 1 | 4.294 | - |
| t-03-write-vs-rewrite | 3 | 35 | 1 | 10 | 0 | 19.209 | - |
| t-04-process-vs-blocking | 5 | 80 | 1 | 7 | 3 | 10.348 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 6.449 | - |

### Failures

- **c-02-sorting-multi** (40/100): final answer failed scenario checks
- **p-03-verify-after-action** (21/100): oracle not matched: 0/2 steps (bullseye 0)
- **t-03-compile-run-cpp** (46/100): final answer failed scenario checks
- **t-03-write-vs-rewrite** (35/100): final answer failed scenario checks

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 124 | 4.96 |
| tool failures | 9 | 0.36 |
| tool denials | 0 | 0 |
| redundant calls | 32 | 1.28 |
| LLM retries | 0 | 0 |
| wall time (s) | 497.099 | 19.884 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 124 |
| total deviation (extra calls) | 75 |
| plan efficiency | 38/100 |
| agentic score (mean plan adherence) | 63/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 41 | 26 | 36 |
| process_read | 1 | 4 | 3 | 25 |
| process_start | 1 | 1 | 0 | 100 |
| read | 18 | 60 | 42 | 30 |
| search | 1 | 1 | 0 | 100 |
| write | 12 | 16 | 4 | 75 |

## gemma4-12b-q4

- run: `run-1785776685335860326` [live, engine 0.3.1, reasoning on (server preset, client auto)]
- **model score: 726/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 84 | 1 | 12 | 10 | 15.356 | 1 |
| c-02-sorting-multi | 3 | 95 | 1 | 8 | 6 | 85.787 | 1 |
| c-03-ring-buffer | 4 | 86 | 1 | 8 | 6 | 16.1 | 1 |
| c-04-lcs | 4 | 90 | 1 | 6 | 4 | 8.744 | 1 |
| c-05-graph-bfs | 5 | 90 | 1 | 6 | 4 | 9.401 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 2.504 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 2.722 | - |
| p-03-verify-after-action | 3 | 21 | 0 | 4 | 3 | 4.523 | - |
| p-05-multi-constraint | 4 | 48 | 1 | 2 | 0 | 3.157 | - |
| p-06-verify-before-claim | 4 | 98 | 1 | 2 | 0 | 2.14 | - |
| r-01-extract-method | 4 | 40 | 1 | 5 | 3 | 9.576 | 0 |
| r-02-polymorphism-over-switch | 4 | 89 | 1 | 9 | 7 | 92.498 | 1 |
| r-03-adapter | 4 | 27 | 1 | 13 | 11 | 84.313 | 0 |
| r-04-strategy | 5 | 34 | 1 | 7 | 5 | 12.387 | 0 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 2.006 | - |
| t-02-grep-extract-value | 3 | 100 | 1 | 2 | 0 | 1.85 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 6.338 | - |
| t-04-env-inventory | 3 | 44 | 1 | 2 | 0 | 10.569 | - |
| t-05-pipeline-transform | 4 | 46 | 1 | 4 | 0 | 8.579 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 2.111 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 6.951 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 3.349 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 5.287 | - |
| t-04-process-vs-blocking | 5 | 32 | 1 | 12 | 9 | 14.038 | - |
| t-06-write-verify-loop | 4 | 82 | 1 | 5 | 2 | 3.789 | - |

### Failures

- **p-03-verify-after-action** (21/100): oracle not matched: 0/2 steps (bullseye 0)
- **p-05-multi-constraint** (48/100): final answer failed scenario checks
- **r-01-extract-method** (40/100): artifact failed to compile; hidden tests failed; artifact behavior differs from reference
- **r-03-adapter** (27/100): artifact failed to compile; hidden tests failed; artifact behavior differs from reference
- **r-04-strategy** (34/100): artifact failed to compile; hidden tests failed; artifact behavior differs from reference
- **t-04-env-inventory** (44/100): final answer failed scenario checks
- **t-05-pipeline-transform** (46/100): final answer failed scenario checks
- **t-04-process-vs-blocking** (32/100): final answer failed scenario checks

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 103 | 4.12 |
| tool failures | 8 | 0.32 |
| tool denials | 0 | 0 |
| redundant calls | 12 | 0.48 |
| LLM retries | 3 | 0.12 |
| wall time (s) | 414.075 | 16.563 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 103 |
| total deviation (extra calls) | 54 |
| plan efficiency | 46/100 |
| agentic score (mean plan adherence) | 70/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 35 | 20 | 42 |
| process_read | 1 | 4 | 3 | 25 |
| process_start | 1 | 2 | 1 | 50 |
| read | 18 | 41 | 23 | 43 |
| search | 1 | 1 | 0 | 100 |
| write | 12 | 18 | 6 | 66 |
| process_stop | 0 | 1 | 1 | 0 |

## gemma4-12b-q4 (reasoning explicit)

- run: `run-1785791432528704324` [live, engine 0.3.1, reasoning on]
- **model score: 799/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 86 | 1 | 11 | 9 | 16.44 | 1 |
| c-02-sorting-multi | 3 | 40 | 1 | 6 | 4 | 15.528 | 0 |
| c-03-ring-buffer | 4 | 34 | 1 | 7 | 5 | 291.758 | 0 |
| c-04-lcs | 4 | 90 | 1 | 6 | 4 | 13.765 | 1 |
| c-05-graph-bfs | 5 | 90 | 1 | 6 | 4 | 9.297 | 1 |
| p-01-envelope-format | 2 | 96 | 1 | 3 | 1 | 4.069 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 3.78 | - |
| p-03-verify-after-action | 3 | 19 | 0 | 5 | 4 | 5.799 | - |
| p-05-multi-constraint | 4 | 48 | 1 | 2 | 0 | 3.37 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 2.744 | - |
| r-01-extract-method | 4 | 100 | 1 | 7 | 5 | 13.038 | 1 |
| r-02-polymorphism-over-switch | 4 | 81 | 1 | 11 | 9 | 87.32 | 1 |
| r-03-adapter | 4 | 80 | 1 | 15 | 13 | 21.315 | 1 |
| r-04-strategy | 5 | 90 | 1 | 6 | 4 | 17.765 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 2.944 | - |
| t-02-grep-extract-value | 3 | 100 | 1 | 3 | 0 | 3.85 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 5.4 | - |
| t-04-env-inventory | 3 | 100 | 1 | 2 | 0 | 5.989 | - |
| t-05-pipeline-transform | 4 | 98 | 1 | 6 | 0 | 19.716 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 2.171 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 4.063 | - |
| t-02-read-vs-cat | 2 | 92 | 1 | 3 | 1 | 5.396 | - |
| t-03-write-vs-rewrite | 3 | 84 | 1 | 5 | 2 | 4.986 | - |
| t-04-process-vs-blocking | 5 | 26 | 0.666667 | 12 | 10 | 16.077 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 4.269 | - |

### Failures

- **c-02-sorting-multi** (40/100): artifact failed to compile; hidden tests failed; artifact behavior differs from reference
- **c-03-ring-buffer** (34/100): final answer failed scenario checks; hidden tests failed; artifact behavior differs from reference
- **p-03-verify-after-action** (19/100): oracle not matched: 0/2 steps (bullseye 0)
- **p-05-multi-constraint** (48/100): final answer failed scenario checks
- **t-04-process-vs-blocking** (26/100): oracle not matched: 2/3 steps (bullseye 0.666667); final answer failed scenario checks

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 111 | 4.44 |
| tool failures | 14 | 0.56 |
| tool denials | 0 | 0 |
| redundant calls | 18 | 0.72 |
| LLM retries | 1 | 0.04 |
| wall time (s) | 580.849 | 23.234 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 111 |
| total deviation (extra calls) | 62 |
| plan efficiency | 43/100 |
| agentic score (mean plan adherence) | 66/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 31 | 16 | 48 |
| process_read | 1 | 6 | 5 | 16 |
| process_start | 1 | 3 | 2 | 33 |
| read | 18 | 47 | 29 | 38 |
| search | 1 | 1 | 0 | 100 |
| write | 12 | 21 | 9 | 57 |
| process_stop | 0 | 1 | 1 | 0 |

## gemma4-31b

- run: `run-1785779734693544183` [live, engine 0.3.1, reasoning on]
- **model score: 903/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 100 | 1 | 8 | 6 | 38.567 | 1 |
| c-02-sorting-multi | 3 | 90 | 1 | 13 | 11 | 262.809 | 1 |
| c-03-ring-buffer | 4 | 80 | 1 | 12 | 10 | 228.383 | 1 |
| c-04-lcs | 4 | 84 | 1 | 9 | 7 | 57.432 | 1 |
| c-05-graph-bfs | 5 | 84 | 1 | 9 | 7 | 71.062 | 1 |
| p-01-envelope-format | 2 | 96 | 1 | 3 | 1 | 8.057 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 6.968 | - |
| p-03-verify-after-action | 3 | 29 | 0 | 2 | 1 | 8.338 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 10.582 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 7.964 | - |
| r-01-extract-method | 4 | 100 | 1 | 7 | 5 | 75.892 | 1 |
| r-02-polymorphism-over-switch | 4 | 100 | 1 | 6 | 4 | 59.186 | 1 |
| r-03-adapter | 4 | 80 | 1 | 11 | 9 | 50.326 | 1 |
| r-04-strategy | 5 | 82 | 1 | 8 | 6 | 51.016 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 6.299 | - |
| t-02-grep-extract-value | 3 | 100 | 1 | 2 | 0 | 10.097 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 3 | 0 | 9.542 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 7.664 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 3 | 0 | 15.391 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 8.393 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 7.068 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 7.096 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 9.319 | - |
| t-04-process-vs-blocking | 5 | 86 | 1 | 6 | 2 | 25.491 | - |
| t-06-write-verify-loop | 4 | 82 | 1 | 5 | 2 | 13.551 | - |

### Failures

- **p-03-verify-after-action** (29/100): oracle not matched: 0/2 steps (bullseye 0)

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 103 | 4.12 |
| tool failures | 2 | 0.08 |
| tool denials | 0 | 0 |
| redundant calls | 5 | 0.2 |
| LLM retries | 0 | 0 |
| wall time (s) | 1056.49 | 42.2597 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 103 |
| total deviation (extra calls) | 54 |
| plan efficiency | 46/100 |
| agentic score (mean plan adherence) | 72/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 39 | 24 | 38 |
| process_read | 1 | 2 | 1 | 50 |
| process_start | 1 | 1 | 0 | 100 |
| read | 18 | 38 | 20 | 47 |
| search | 1 | 1 | 0 | 100 |
| write | 12 | 21 | 9 | 57 |

## kilo-auto/free

- run: `run-1785839903363081822` [live, engine 0.3.1, reasoning on]
- **model score: 861/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 100 | 1 | 6 | 5 | 14.267 | 1 |
| c-02-sorting-multi | 3 | 100 | 1 | 9 | 8 | 25.212 | 1 |
| c-03-ring-buffer | 4 | 80 | 1 | 9 | 9 | 33.581 | 1 |
| c-04-lcs | 4 | 84 | 1 | 9 | 8 | 24.631 | 1 |
| c-05-graph-bfs | 5 | 82 | 1 | 8 | 8 | 16.609 | 1 |
| p-01-envelope-format | 2 | 96 | 1 | 3 | 1 | 7.765 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 5.317 | - |
| p-03-verify-after-action | 3 | 25 | 0 | 3 | 2 | 16.492 | - |
| p-05-multi-constraint | 4 | 48 | 1 | 2 | 0 | 13.687 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 5.191 | - |
| r-01-extract-method | 4 | 96 | 1 | 7 | 8 | 26.082 | 1 |
| r-02-polymorphism-over-switch | 4 | 88 | 1 | 12 | 13 | 30.029 | 1 |
| r-03-adapter | 4 | 82 | 1 | 8 | 10 | 23.754 | 1 |
| r-04-strategy | 5 | 82 | 1 | 8 | 8 | 21.773 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 5.149 | - |
| t-02-grep-extract-value | 3 | 100 | 1 | 3 | 0 | 5.903 | - |
| t-03-compile-run-cpp | 3 | 46 | 1 | 3 | 0 | 5.723 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 3.028 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 4 | 0 | 15.994 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 18.304 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 5.474 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 4.673 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 12.806 | - |
| t-04-process-vs-blocking | 5 | 90 | 1 | 5 | 1 | 14.252 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 8.845 | - |

### Failures

- **p-03-verify-after-action** (25/100): oracle not matched: 0/2 steps (bullseye 0)
- **p-05-multi-constraint** (48/100): final answer failed scenario checks
- **t-03-compile-run-cpp** (46/100): final answer failed scenario checks

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 115 | 4.6 |
| tool failures | 7 | 0.28 |
| tool denials | 0 | 0 |
| redundant calls | 8 | 0.32 |
| LLM retries | 0 | 0 |
| wall time (s) | 364.541 | 14.5816 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 115 |
| total deviation (extra calls) | 66 |
| plan efficiency | 41/100 |
| agentic score (mean plan adherence) | 63/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 51 | 36 | 29 |
| process_read | 1 | 2 | 1 | 50 |
| process_start | 1 | 1 | 0 | 100 |
| read | 18 | 44 | 26 | 40 |
| search | 1 | 1 | 0 | 100 |
| write | 12 | 15 | 3 | 80 |

## NVIDIA Nemotron 3 Ultra 550B (free)

- run: `run-1785840286887120700` [live, engine 0.3.1, reasoning on]
- **model score: 806/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 88 | 1 | 10 | 8 | 63.17 | 1 |
| c-02-sorting-multi | 3 | 82 | 1 | 13 | 12 | 138.957 | 1 |
| c-03-ring-buffer | 4 | 40 | 1 | 20 | 19 | 155.896 | 1 |
| c-04-lcs | 4 | 80 | 1 | 9 | 8 | 66.522 | 1 |
| c-05-graph-bfs | 5 | 80 | 1 | 10 | 8 | 46.992 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 6.236 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 15.068 | - |
| p-03-verify-after-action | 3 | 29 | 0 | 2 | 1 | 7.961 | - |
| p-05-multi-constraint | 4 | 48 | 1 | 2 | 0 | 18.936 | - |
| p-06-verify-before-claim | 4 | 98 | 1 | 2 | 0 | 38.914 | - |
| r-01-extract-method | 4 | 96 | 1 | 6 | 4 | 77.26 | 1 |
| r-02-polymorphism-over-switch | 4 | 94 | 1 | 9 | 7 | 71.458 | 1 |
| r-03-adapter | 4 | 80 | 1 | 11 | 14 | 116.425 | 1 |
| r-04-strategy | 5 | 82 | 1 | 8 | 7 | 50.312 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 6.888 | - |
| t-02-grep-extract-value | 3 | 100 | 1 | 2 | 0 | 11.287 | - |
| t-03-compile-run-cpp | 3 | 46 | 1 | 4 | 0 | 36.488 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 8.686 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 4 | 0 | 41.351 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 6.936 | - |
| t-01-search-vs-bash-grep | 3 | 30 | 1 | 10 | 8 | 32.122 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 5.631 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 13.98 | - |
| t-04-process-vs-blocking | 5 | 80 | 1 | 8 | 4 | 31.512 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 18.485 | - |

### Failures

- **c-03-ring-buffer** (40/100): final answer failed scenario checks
- **p-03-verify-after-action** (29/100): oracle not matched: 0/2 steps (bullseye 0)
- **p-05-multi-constraint** (48/100): final answer failed scenario checks
- **t-03-compile-run-cpp** (46/100): final answer failed scenario checks
- **t-01-search-vs-bash-grep** (30/100): final answer failed scenario checks

### Agentic profile

| metric | total | per scenario |
|---|---|---|
| tool calls | 133 | 5.32 |
| tool failures | 8 | 0.32 |
| tool denials | 0 | 0 |
| redundant calls | 20 | 0.8 |
| LLM retries | 0 | 0 |
| wall time (s) | 1087.47 | 43.4989 |

**Plan adherence** (optimal tool plan vs actual):

| metric | value |
|---|---|
| scenarios with a plan | 24 |
| optimal tool calls (sum) | 48 |
| actual tool calls | 133 |
| total deviation (extra calls) | 84 |
| plan efficiency | 36/100 |
| agentic score (mean plan adherence) | 58/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation | efficiency % |
|---|---|---|---|---|
| bash | 15 | 46 | 31 | 32 |
| process_read | 1 | 3 | 2 | 33 |
| process_start | 1 | 2 | 1 | 50 |
| read | 18 | 57 | 39 | 31 |
| search | 1 | 10 | 9 | 10 |
| write | 12 | 14 | 2 | 85 |


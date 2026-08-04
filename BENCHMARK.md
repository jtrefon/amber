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

| model | score | agentic | tools | failures | denied | redundant | retries | steps | wall (s) |
|---|---|---|---|---|---|---|---|---|---|
| qwopus-27b | 900 | 62 | 109 | 5 | 0 | 17 | 0 | 121 | 321.902 |
| Qwen3.6-27B dense | 910 | 68 | 104 | 4 | 0 | 10 | 0 | 115 | 781.901 |
| Qwen3.6-27B MTP | 901 | 66 | 104 | 2 | 0 | 12 | 0 | 117 | 308.714 |
| Qwen3.6-35B MoE (A3B) | 877 | 59 | 137 | 12 | 1 | 22 | 0 | 145 | 548.405 |
| ornith-1.0-35b | 837 | 63 | 124 | 9 | 0 | 32 | 0 | 153 | 497.099 |
| gemma4-12b-q4 | 726 | 70 | 103 | 8 | 0 | 12 | 3 | 127 | 414.075 |
| gemma4-12b-q4 (reasoning explicit) | 799 | 66 | 111 | 14 | 0 | 18 | 1 | 135 | 580.849 |
| gemma4-31b | 903 | 72 | 103 | 2 | 0 | 5 | 0 | 127 | 1056.49 |

| scenario | qwopus-27b | Qwen3.6-27B dense | Qwen3.6-27B MTP | Qwen3.6-35B MoE (A3B) | ornith-1.0-35b | gemma4-12b-q4 | gemma4-12b-q4 (reasoning explicit) | gemma4-31b |
|---|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 96 | 96 | 96 | 100 | 96 | 84 | 86 | 100 |
| c-02-sorting-multi | 96 | 96 | 100 | 40 | 40 | 95 | 40 | 90 |
| c-03-ring-buffer | 84 | 80 | 80 | 80 | 84 | 86 | 34 | 80 |
| c-04-lcs | 80 | 88 | 84 | 80 | 84 | 90 | 90 | 84 |
| c-05-graph-bfs | 84 | 86 | 80 | 82 | 82 | 90 | 90 | 84 |
| p-01-envelope-format | 100 | 100 | 100 | 100 | 96 | 100 | 96 | 96 |
| p-02-banned-tool-refusal | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| p-03-verify-after-action | 15 | 27 | 21 | 25 | 21 | 21 | 19 | 29 |
| p-05-multi-constraint | 100 | 100 | 100 | 100 | 100 | 48 | 48 | 100 |
| p-06-verify-before-claim | 98 | 94 | 94 | 98 | 94 | 98 | 94 | 94 |
| r-01-extract-method | 96 | 94 | 96 | 92 | 96 | 40 | 100 | 100 |
| r-02-polymorphism-over-switch | 92 | 86 | 96 | 80 | 96 | 89 | 81 | 100 |
| r-03-adapter | 80 | 82 | 80 | 86 | 80 | 27 | 80 | 80 |
| r-04-strategy | 88 | 88 | 86 | 84 | 80 | 34 | 90 | 82 |
| t-01-ls-count-files | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| t-02-grep-extract-value | 100 | 98 | 100 | 100 | 98 | 100 | 100 | 100 |
| t-03-compile-run-cpp | 100 | 100 | 100 | 100 | 46 | 100 | 100 | 100 |
| t-04-env-inventory | 100 | 100 | 100 | 100 | 100 | 44 | 100 | 100 |
| t-05-pipeline-transform | 100 | 100 | 100 | 98 | 100 | 46 | 98 | 100 |
| t-06-multi-dir-count | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| t-01-search-vs-bash-grep | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| t-02-read-vs-cat | 100 | 100 | 100 | 100 | 94 | 100 | 92 | 100 |
| t-03-write-vs-rewrite | 92 | 100 | 92 | 100 | 35 | 92 | 84 | 92 |
| t-04-process-vs-blocking | 80 | 90 | 82 | 80 | 80 | 32 | 26 | 86 |
| t-06-write-verify-loop | 90 | 90 | 90 | 90 | 90 | 82 | 90 | 82 |

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
| agentic score (mean plan adherence) | 62/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation |
|---|---|---|---|
| bash | 15 | 39 | 24 |
| process_read | 1 | 3 | 2 |
| process_start | 1 | 1 | 0 |
| read | 18 | 49 | 31 |
| search | 1 | 1 | 0 |
| write | 12 | 15 | 3 |

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
| agentic score (mean plan adherence) | 68/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation |
|---|---|---|---|
| bash | 15 | 36 | 21 |
| process_read | 1 | 2 | 1 |
| process_start | 1 | 1 | 0 |
| read | 18 | 48 | 30 |
| search | 1 | 2 | 1 |
| write | 12 | 14 | 2 |

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
| agentic score (mean plan adherence) | 66/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation |
|---|---|---|---|
| bash | 15 | 31 | 16 |
| process_read | 1 | 2 | 1 |
| process_start | 1 | 1 | 0 |
| read | 18 | 52 | 34 |
| search | 1 | 1 | 0 |
| write | 12 | 15 | 3 |

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
| agentic score (mean plan adherence) | 59/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation |
|---|---|---|---|
| bash | 15 | 44 | 29 |
| process_read | 1 | 3 | 2 |
| process_start | 1 | 1 | 0 |
| read | 18 | 60 | 42 |
| search | 1 | 1 | 0 |
| write | 12 | 27 | 15 |

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
| agentic score (mean plan adherence) | 63/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation |
|---|---|---|---|
| bash | 15 | 41 | 26 |
| process_read | 1 | 4 | 3 |
| process_start | 1 | 1 | 0 |
| read | 18 | 60 | 42 |
| search | 1 | 1 | 0 |
| write | 12 | 16 | 4 |

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
| agentic score (mean plan adherence) | 70/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation |
|---|---|---|---|
| bash | 15 | 35 | 20 |
| process_read | 1 | 4 | 3 |
| process_start | 1 | 2 | 1 |
| read | 18 | 41 | 23 |
| search | 1 | 1 | 0 |
| write | 12 | 18 | 6 |
| process_stop | 0 | 1 | 1 |

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
| agentic score (mean plan adherence) | 66/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation |
|---|---|---|---|
| bash | 15 | 31 | 16 |
| process_read | 1 | 6 | 5 |
| process_start | 1 | 3 | 2 |
| read | 18 | 47 | 29 |
| search | 1 | 1 | 0 |
| write | 12 | 21 | 9 |
| process_stop | 0 | 1 | 1 |

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
| agentic score (mean plan adherence) | 72/100 |

**Tool mix (plan vs actual, summed across scenarios):**

| tool | plan | actual | deviation |
|---|---|---|---|
| bash | 15 | 39 | 24 |
| process_read | 1 | 2 | 1 |
| process_start | 1 | 1 | 0 |
| read | 18 | 38 | 20 |
| search | 1 | 1 | 0 |
| write | 12 | 21 | 9 |


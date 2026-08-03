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
./amber-bench run --live --profile qwen35-dense --out .amber/bench/results/qwen35-dense.json
./amber-bench run --live --profile ornith-35b   --out .amber/bench/results/ornith-35b.json
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
# Benchmark: harness score by model

- **qwopus-27b**: 841/1000
- **qwen35-dense**: 910/1000
- **ornith-1.0-35b**: 837/1000
- **gemma4-12b-q4**: 702/1000
- **gemma4-31b**: 878/1000

| scenario | qwopus-27b | qwen35-dense | ornith-1.0-35b | gemma4-12b-q4 | gemma4-31b |
|---|---|---|---|---|---|
| c-01-fizzbuzz | 96 | 96 | 96 | 84 | 100 |
| c-02-sorting-multi | 96 | 96 | 40 | 95 | 90 |
| c-03-ring-buffer | 80 | 80 | 84 | 86 | 80 |
| c-04-lcs | 92 | 88 | 84 | 90 | 84 |
| c-05-graph-bfs | 86 | 86 | 82 | 90 | 84 |
| p-01-envelope-format | 100 | 100 | 96 | 100 | 96 |
| p-02-banned-tool-refusal | 100 | 100 | 100 | 100 | 100 |
| p-03-verify-after-action | 25 | 27 | 21 | 21 | 29 |
| p-05-multi-constraint | 100 | 100 | 100 | 48 | 100 |
| p-06-verify-before-claim | 94 | 94 | 94 | 98 | 94 |
| r-01-extract-method | 96 | 94 | 96 | 40 | 100 |
| r-02-polymorphism-over-switch | 96 | 86 | 96 | 89 | 100 |
| r-03-adapter | 80 | 82 | 80 | 27 | 80 |
| r-04-strategy | 88 | 88 | 80 | 34 | 82 |
| t-01-ls-count-files | 100 | 100 | 100 | 100 | 100 |
| t-02-grep-extract-value | 30 | 98 | 98 | 30 | 28 |
| t-03-compile-run-cpp | 100 | 100 | 46 | 100 | 100 |
| t-04-env-inventory | 100 | 100 | 100 | 44 | 100 |
| t-05-pipeline-transform | 100 | 100 | 100 | 46 | 100 |
| t-06-multi-dir-count | 100 | 100 | 100 | 100 | 100 |
| t-01-search-vs-bash-grep | 100 | 100 | 100 | 100 | 100 |
| t-02-read-vs-cat | 31 | 100 | 94 | 100 | 100 |
| t-03-write-vs-rewrite | 92 | 100 | 35 | 92 | 92 |
| t-04-process-vs-blocking | 34 | 90 | 80 | 32 | 86 |
| t-06-write-verify-loop | 90 | 90 | 90 | 82 | 82 |

---

## qwopus-27b

- run: `run-1785777163134501929` [live, engine 0.3.1, reasoning default]
- **model score: 841/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 96 | 1 | 7 | 6 | 13.844 | 1 |
| c-02-sorting-multi | 3 | 96 | 1 | 8 | 7 | 25.751 | 1 |
| c-03-ring-buffer | 4 | 80 | 1 | 14 | 13 | 42.586 | 1 |
| c-04-lcs | 4 | 92 | 1 | 5 | 3 | 15.518 | 1 |
| c-05-graph-bfs | 5 | 86 | 1 | 6 | 6 | 18.15 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 4.8 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 4.825 | - |
| p-03-verify-after-action | 3 | 25 | 0 | 3 | 2 | 7.183 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 5.23 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 5.748 | - |
| r-01-extract-method | 4 | 96 | 1 | 6 | 5 | 18.729 | 1 |
| r-02-polymorphism-over-switch | 4 | 96 | 1 | 5 | 5 | 16.472 | 1 |
| r-03-adapter | 4 | 80 | 1 | 9 | 10 | 18.045 | 1 |
| r-04-strategy | 5 | 88 | 1 | 5 | 5 | 15.66 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 5.06 | - |
| t-02-grep-extract-value | 3 | 30 | 0 | 2 | 1 | 4.765 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 7.202 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 4.74 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 4 | 0 | 8.343 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 3 | 0 | 7.282 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 4.896 | - |
| t-02-read-vs-cat | 2 | 31 | 0 | 2 | 1 | 5.469 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 7.034 | - |
| t-04-process-vs-blocking | 5 | 34 | 0.666667 | 7 | 4 | 20.192 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 6.417 | - |

### Failures

- **p-03-verify-after-action** (25/100): oracle not matched: 0/2 steps (bullseye 0)
- **t-02-grep-extract-value** (30/100): oracle not matched: 0/1 steps (bullseye 0)
- **t-02-read-vs-cat** (31/100): oracle not matched: 0/1 steps (bullseye 0)
- **t-04-process-vs-blocking** (34/100): oracle not matched: 2/3 steps (bullseye 0.666667)

## qwen35-dense

- run: `run-1785781966685651020` [live, engine 0.3.1, reasoning default]
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

## gemma4-12b-q4

- run: `run-1785776685335860326` [live, engine 0.3.1, reasoning default]
- **model score: 702/1000** (25 scenarios)

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
| t-02-grep-extract-value | 3 | 30 | 0 | 2 | 1 | 1.85 | - |
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
- **t-02-grep-extract-value** (30/100): oracle not matched: 0/1 steps (bullseye 0)
- **t-04-env-inventory** (44/100): final answer failed scenario checks
- **t-05-pipeline-transform** (46/100): final answer failed scenario checks
- **t-04-process-vs-blocking** (32/100): final answer failed scenario checks

## gemma4-31b

- run: `run-1785779734693544183` [live, engine 0.3.1, reasoning default]
- **model score: 878/1000** (25 scenarios)

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
| t-02-grep-extract-value | 3 | 28 | 0 | 3 | 2 | 10.097 | - |
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
- **t-02-grep-extract-value** (28/100): oracle not matched: 0/1 steps (bullseye 0)


# Benchmark

**What this is**: a KPI record of the *amber harness* — how well the agent
loop, tool schemas and prompts drive each model to the correct solution. This
is a harness score, not a model leaderboard: every scenario runs through the
same engine, and the score measures the harness+model pair over 31 scenarios
across 6 suites (agent failures, terminal work, tool selection, prompt
adherence, coding, refactoring).

**How to reproduce** (requires a running OpenAI-compatible server, e.g.
llama.cpp / llama-turboq with the presets below):

```sh
./amber-bench run --live --profile qwopus-27b   --out .amber/bench/results/qwopus-27b.json
./amber-bench run --live --profile gemma4-12b-q4 --out .amber/bench/results/gemma4-12b-q4.json
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

- **qwopus-27b**: 888/1000
- **ornith-1.0-35b**: 887/1000
- **gemma4-12b-q4**: 850/1000

| scenario | qwopus-27b | ornith-1.0-35b | gemma4-12b-q4 |
|---|---|---|---|
| c-01-fizzbuzz | 96 | 96 | 84 |
| c-02-sorting-multi | 96 | 80 | 95 |
| c-03-ring-buffer | 80 | 84 | 86 |
| c-04-lcs | 92 | 84 | 90 |
| c-05-graph-bfs | 86 | 82 | 90 |
| p-01-envelope-format | 100 | 96 | 100 |
| p-02-banned-tool-refusal | 100 | 100 | 100 |
| p-03-verify-after-action | 51 | 43 | 43 |
| p-05-multi-constraint | 100 | 100 | 97 |
| p-06-verify-before-claim | 94 | 94 | 98 |
| r-01-extract-method | 96 | 96 | 80 |
| r-02-polymorphism-over-switch | 96 | 96 | 89 |
| r-03-adapter | 80 | 80 | 55 |
| r-04-strategy | 88 | 80 | 68 |
| t-01-ls-count-files | 100 | 100 | 100 |
| t-02-grep-extract-value | 61 | 98 | 61 |
| t-03-compile-run-cpp | 100 | 92 | 100 |
| t-04-env-inventory | 100 | 100 | 88 |
| t-05-pipeline-transform | 100 | 100 | 92 |
| t-06-multi-dir-count | 100 | 100 | 100 |
| t-01-search-vs-bash-grep | 100 | 100 | 100 |
| t-02-read-vs-cat | 63 | 94 | 100 |
| t-03-write-vs-rewrite | 92 | 71 | 92 |
| t-04-process-vs-blocking | 68 | 80 | 65 |
| t-06-write-verify-loop | 90 | 90 | 82 |

---

## qwopus-27b

- run: `run-1785777163134501929` [live, engine 0.3.1]
- **model score: 888/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 96 | 1 | 7 | 6 | 13.844 | 1 |
| c-02-sorting-multi | 3 | 96 | 1 | 8 | 7 | 25.751 | 1 |
| c-03-ring-buffer | 4 | 80 | 1 | 14 | 13 | 42.586 | 1 |
| c-04-lcs | 4 | 92 | 1 | 5 | 3 | 15.518 | 1 |
| c-05-graph-bfs | 5 | 86 | 1 | 6 | 6 | 18.15 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 4.8 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 4.825 | - |
| p-03-verify-after-action | 3 | 51 | 0 | 3 | 2 | 7.183 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 5.23 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 5.748 | - |
| r-01-extract-method | 4 | 96 | 1 | 6 | 5 | 18.729 | 1 |
| r-02-polymorphism-over-switch | 4 | 96 | 1 | 5 | 5 | 16.472 | 1 |
| r-03-adapter | 4 | 80 | 1 | 9 | 10 | 18.045 | 1 |
| r-04-strategy | 5 | 88 | 1 | 5 | 5 | 15.66 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 5.06 | - |
| t-02-grep-extract-value | 3 | 61 | 0 | 2 | 1 | 4.765 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 7.202 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 4.74 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 4 | 0 | 8.343 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 3 | 0 | 7.282 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 4.896 | - |
| t-02-read-vs-cat | 2 | 63 | 0 | 2 | 1 | 5.469 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 7.034 | - |
| t-04-process-vs-blocking | 5 | 68 | 0.666667 | 7 | 4 | 20.192 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 6.417 | - |

### Failures

- **p-03-verify-after-action** (51/100): oracle not matched: 0/2 steps (bullseye 0)
- **t-02-grep-extract-value** (61/100): oracle not matched: 0/1 steps (bullseye 0)
- **t-02-read-vs-cat** (63/100): oracle not matched: 0/1 steps (bullseye 0)
- **t-04-process-vs-blocking** (68/100): oracle not matched: 2/3 steps (bullseye 0.666667)

## ornith-1.0-35b

- run: `run-1785777586223645316` [live, engine 0.3.1]
- **model score: 887/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 96 | 1 | 7 | 6 | 16.242 | 1 |
| c-02-sorting-multi | 3 | 80 | 1 | 20 | 20 | 135.772 | 1 |
| c-03-ring-buffer | 4 | 84 | 1 | 7 | 6 | 23.121 | 1 |
| c-04-lcs | 4 | 84 | 1 | 7 | 6 | 15.622 | 1 |
| c-05-graph-bfs | 5 | 82 | 1 | 8 | 7 | 24.12 | 1 |
| p-01-envelope-format | 2 | 96 | 1 | 3 | 1 | 5.097 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 3.408 | - |
| p-03-verify-after-action | 3 | 43 | 0 | 4 | 3 | 7.216 | - |
| p-05-multi-constraint | 4 | 100 | 1 | 2 | 0 | 4.073 | - |
| p-06-verify-before-claim | 4 | 94 | 1 | 3 | 1 | 4.889 | - |
| r-01-extract-method | 4 | 96 | 1 | 6 | 5 | 23.923 | 1 |
| r-02-polymorphism-over-switch | 4 | 96 | 1 | 8 | 7 | 28.272 | 1 |
| r-03-adapter | 4 | 80 | 1 | 16 | 7 | 50.043 | 1 |
| r-04-strategy | 5 | 80 | 1 | 20 | 19 | 83.871 | 1 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 3.49 | - |
| t-02-grep-extract-value | 3 | 98 | 1 | 2 | 0 | 3.276 | - |
| t-03-compile-run-cpp | 3 | 92 | 1 | 4 | 0 | 6.752 | - |
| t-04-env-inventory | 3 | 100 | 1 | 1 | 0 | 2.942 | - |
| t-05-pipeline-transform | 4 | 100 | 1 | 4 | 0 | 7.482 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 3.709 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 3.479 | - |
| t-02-read-vs-cat | 2 | 94 | 1 | 2 | 1 | 4.294 | - |
| t-03-write-vs-rewrite | 3 | 71 | 1 | 10 | 0 | 19.209 | - |
| t-04-process-vs-blocking | 5 | 80 | 1 | 7 | 3 | 10.348 | - |
| t-06-write-verify-loop | 4 | 90 | 1 | 4 | 1 | 6.449 | - |

### Failures

- **c-02-sorting-multi** (80/100): final answer failed scenario checks
- **p-03-verify-after-action** (43/100): oracle not matched: 0/2 steps (bullseye 0)
- **t-03-compile-run-cpp** (92/100): final answer failed scenario checks
- **t-03-write-vs-rewrite** (71/100): final answer failed scenario checks

## gemma4-12b-q4

- run: `run-1785776685335860326` [live, engine 0.3.1]
- **model score: 850/1000** (25 scenarios)

| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |
|---|---|---|---|---|---|---|---|
| c-01-fizzbuzz | 2 | 84 | 1 | 12 | 10 | 15.356 | 1 |
| c-02-sorting-multi | 3 | 95 | 1 | 8 | 6 | 85.787 | 1 |
| c-03-ring-buffer | 4 | 86 | 1 | 8 | 6 | 16.1 | 1 |
| c-04-lcs | 4 | 90 | 1 | 6 | 4 | 8.744 | 1 |
| c-05-graph-bfs | 5 | 90 | 1 | 6 | 4 | 9.401 | 1 |
| p-01-envelope-format | 2 | 100 | 1 | 2 | 0 | 2.504 | - |
| p-02-banned-tool-refusal | 3 | 100 | 1 | 2 | 0 | 2.722 | - |
| p-03-verify-after-action | 3 | 43 | 0 | 4 | 3 | 4.523 | - |
| p-05-multi-constraint | 4 | 97 | 1 | 2 | 0 | 3.157 | - |
| p-06-verify-before-claim | 4 | 98 | 1 | 2 | 0 | 2.14 | - |
| r-01-extract-method | 4 | 80 | 1 | 5 | 3 | 9.576 | 0 |
| r-02-polymorphism-over-switch | 4 | 89 | 1 | 9 | 7 | 92.498 | 1 |
| r-03-adapter | 4 | 55 | 1 | 13 | 11 | 84.313 | 0 |
| r-04-strategy | 5 | 68 | 1 | 7 | 5 | 12.387 | 0 |
| t-01-ls-count-files | 2 | 100 | 1 | 2 | 0 | 2.006 | - |
| t-02-grep-extract-value | 3 | 61 | 0 | 2 | 1 | 1.85 | - |
| t-03-compile-run-cpp | 3 | 100 | 1 | 4 | 0 | 6.338 | - |
| t-04-env-inventory | 3 | 88 | 1 | 2 | 0 | 10.569 | - |
| t-05-pipeline-transform | 4 | 92 | 1 | 4 | 0 | 8.579 | - |
| t-06-multi-dir-count | 3 | 100 | 1 | 2 | 0 | 2.111 | - |
| t-01-search-vs-bash-grep | 3 | 100 | 1 | 2 | 0 | 6.951 | - |
| t-02-read-vs-cat | 2 | 100 | 1 | 2 | 0 | 3.349 | - |
| t-03-write-vs-rewrite | 3 | 92 | 1 | 4 | 1 | 5.287 | - |
| t-04-process-vs-blocking | 5 | 65 | 1 | 12 | 9 | 14.038 | - |
| t-06-write-verify-loop | 4 | 82 | 1 | 5 | 2 | 3.789 | - |

### Failures

- **p-03-verify-after-action** (43/100): oracle not matched: 0/2 steps (bullseye 0)
- **p-05-multi-constraint** (97/100): final answer failed scenario checks
- **r-01-extract-method** (80/100): artifact failed to compile; hidden tests failed; artifact behavior differs from reference
- **r-03-adapter** (55/100): artifact failed to compile; hidden tests failed; artifact behavior differs from reference
- **r-04-strategy** (68/100): artifact failed to compile; hidden tests failed; artifact behavior differs from reference
- **t-02-grep-extract-value** (61/100): oracle not matched: 0/1 steps (bullseye 0)
- **t-04-env-inventory** (88/100): final answer failed scenario checks
- **t-05-pipeline-transform** (92/100): final answer failed scenario checks
- **t-04-process-vs-blocking** (65/100): final answer failed scenario checks


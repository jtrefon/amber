# Spec: KPI Catalog

Status: **Full-scope proposal — awaiting approval**
Companion: `benchmark/kpi-framework.md` (harness), `benchmark/corpus.md` (corpus).

Every KPI is objective, computed by the harness from the recorder event
stream, run timing, resource sampling, or the static-template engine. Units
and sources are fixed; nothing here requires human judgment.

## 1. Correctness & accuracy

| KPI | Unit | Definition | Source |
|-----|------|------------|--------|
| `success` | bool | Oracle fully matched && all checks passed && within budget && no hard stop | scorer |
| `bullseye_ratio` | 0..1 | oracle steps matched / oracle steps (partial credit) | oracle |
| `bullseye_at_ms` | ms | wall time until the oracle is fully matched (prompt → exactly what we need) | runner |
| `tool_call_accuracy` | 0..1 | on-oracle calls / total tool calls | oracle + recorder |
| `arg_precision` | 0..1 | matched arg keys / expected arg keys, on-oracle calls only | oracle |
| `compile_ok` | bool | agent artifact compiles with declared toolchains | template engine |
| `artifact_score` | 0..1 | hidden tests passed / hidden tests total | template engine |
| `behavior_equivalence` | bool | same hidden tests pass on reference and artifact, outputs byte-identical | template engine |
| `structure_checks` | 0..1 | pattern checks passed / total (must/must-not contain) | template engine |
| `prompt_adherence` | 0..1 | `prompt_checks` passed / total (envelope, banned-tool refusal, verify-after-action) | recorder |
| `hallucination_penalty` | count | final-answer facts contradicted by fixture content (p-04) | checks |

## 2. Efficiency

| KPI | Unit | Definition | Source |
|-----|------|------------|--------|
| `steps_to_solution` | count | tool iterations until run end (loop iterations from `on_debug`) | recorder |
| `steps_to_bullseye` | count | iterations until oracle fully matched | oracle |
| `wasted_calls` | count | off-oracle + failed + denied + forbidden-tool calls | oracle + recorder |
| `redundant_calls` | count | repeated identical (tool, args) calls (f-01) | recorder (fingerprint) |
| `wall_time_ms` | ms | caller-measured around `Agent::run()` | runner |
| `step_time_ms` | ms | avg + p95 of `on_tool_call` → `on_tool_result` pairs | recorder |
| `ttft_ms` | ms | first `on_token` delta after request start | recorder |
| `tps` | tok/s | completion tokens / generation time (`Stats`) | `on_stats` |
| `prompt_tokens` / `completion_tokens` | tok | per request, summed | `on_stats` |

## 3. Robustness & recovery

| KPI | Unit | Definition | Source |
|-----|------|------------|--------|
| `retry_rate` | count | LLM-side retry attempts (retryable errors) | status stream parse |
| `recovery_count` | count | repairs (schema/template/model fixes) + injected steers | status stream parse |
| `loop_steer_count` | count | FailStreak/text-loop steering injections | status stream parse |
| `hard_stops` | count | unrecoverable loop terminations | status stream parse |
| `dropout_recovery` | bool | run completes after a scripted/observed mid-stream dropout | recorder |
| `model_robustness` | 0..1 | success(poor profile) / success(good profile), same corpus | cross-profile agg |
| `error_types` | map | chat_error/tool_recovery/loop categories observed (ConversationLog event types) | log + status parse |

## 4. Resources (engine footprint — relevant for the port)

| KPI | Unit | Definition | Source |
|-----|------|------------|--------|
| `baseline_rss_kb` | KiB | process RSS before the first request (engine idle footprint) | getrusage baseline |
| `peak_rss_kb` | KiB | max RSS during the run (agent + tools) | getrusage during run |
| `cpu_ms` | ms | user + sys CPU consumed during the run | getrusage |
| `startup_ms` | ms | binary launch → first LLM request sent | runner |

Notes:

- **Disk I/O is deliberately NOT a KPI.** For an LLM agent harness the working
  set is tiny (prompts, JSON, small workspace files); disk throughput carries
  no signal. The measurable side-effect proxy is `files_touched` (count +
  bytes) from write-tool `meta.path` — a sloppiness indicator, not I/O load.
- Cross-platform: `getrusage().ru_maxrss` is **bytes on macOS, KiB on Linux**
  — normalize with `__APPLE__` before any comparison. `procfs`-based sampling
  is Linux-only and must never be used; getrusage is POSIX.
- Resource KPIs are measured per scenario in isolation (serial runner) so the
  numbers are comparable; `--repeat` aggregates median/p95.

## 5. Judgment metrics (SOLID / KISS / DRY / YAGNI / DDD / BDD)

These cannot be measured directly — they are code-review properties. The
framework measures objective **proxies** and states which principle each maps
to:

| Principle | Proxy KPI | How |
|-----------|-----------|-----|
| SOLID — OCP/LSP/DIP | `structure_checks` (interface/virtual-base present), `behavior_equivalence` | refactor suite (r-02..r-06) |
| SOLID — SRP | `extract-method` structure checks (named helpers, no monster function) | refactor r-01 |
| Cyclomatic complexity | `static_analysis_findings` (cppcheck/clang-tidy run on the artifact with a fixed config) | refactor r-05 |
| DRY | `duplicate_blocks` (repo's `tools/duplicate_detector.py` run on the artifact) | refactor scenarios |
| KISS / YAGNI | `artifact_size` (LOC of artifact vs reference) + `files_touched` | all coding/refactor |
| DDD / BDD | Framework-internal: scenario files ARE behavior specs (name = behavior, oracle = when, checks = then); the framework itself is developed TDD red→green | process, not product |
| Prompt strength | `tool_call_accuracy` + `bullseye_ratio` per suite | `tools` suite |

## 6. Scoring (continuous, weighted)

Every scenario earns a **continuous score (0..100)** — partial credit on every
component, so weak and strong models separate:

- `correctness` = 0.6·bullseye + 0.4·artifact (template) | 0.7·bullseye + 0.3·checks (else)
- `efficiency` = 100·clamp(1 − 0.10·excess_steps − 0.10·wasted − 0.20·redundant) (template: wasted excluded)
- `robustness` = 100·clamp(1 − 0.30·retries − 0.50·recoveries − 1.00·hard_stop)
- `adherence` = 100·clamp(prompt_adherence − 0.25·forbidden_calls)
- `total` = 0.50·correctness + 0.20·efficiency + 0.15·robustness + 0.15·adherence
- **model score** = difficulty-weighted aggregate (difficulty 1–5 per scenario) → /1000

Budgets are **enforced during the run** (`max_tool_iterations` cap from
`max_steps`), not only scored after. Scenario budgets, difficulty and
`expected_steps` (efficiency baseline) are JSON fields.

## 7. Aggregation & reporting

- **Per scenario**: the KPI record above (one JSON object + event stream file).
- **Per run** (suite × profile): median/p95 of continuous KPIs, sums of
  counts, pass/fail matrix per scenario.
- **Per model comparison** (`--profile good --profile poor`): robustness
  matrix, latency delta, bullseye delta.
- **Trend**: history under `.amber/bench/results/`; `amber-bench report` shows
  deltas vs the previous run (regression alert when success drops or
  bullseye/latency regress beyond a configurable threshold).
- Hermetic runs are deterministic; KPI assertions live in
  `tests/bench_test.cpp` (e.g. scripted fake → exact expected bullseye_ratio,
  steps, retry counts).

## Non-goals

- Semantic similarity of free-form answers (P3 optional; until then checks are
  exact patterns).
- Measuring model quality itself — the framework measures *engine behavior
  given a model*, and model comparison is a derived report.
- Real-model runs in CI (free-model constraint).

# Release Benchmark Procedure

**Purpose:** every release, run the full model population through the harness
benchmark and update the benchmark chart. This is the win/lose/stagnate
signal for the release: harness health, per-model ranking, and the top
failure classes.

## When

At every **engine** release (tag-driven, `vX.Y.Z`) — after the release branch
is cut and before the tag. Docs/website/packaging-only tags skip the live run;
the release workflow decides with the changed-path list and
`make bench-release-check` fails closed when an engine tag has no artifact.

## Minimum for the gate

One live run on the fixed local profile with `--repeat 3`, committed as
`bench/results/releases/<version>/local-<model>-r3.json`. The model corpus is
the current one (56 model scenarios on 2026-11-23; the gate's absolute floor is 49):

```sh
./amber-bench run --live --profile qwen38-27b-mtp --repeat 3 \
  --out bench/results/releases/<version>/local-qwen38-27b-mtp-r3.json
make bench-release-check VERSION=<version>
```

The gate rejects a non-live run, a different engine, a short corpus, an
infrastructure failure (a setup error, a throw, a task that never finished), a
score drop beyond the recorded band, and the cost/behaviour regressions in the
section below. A model losing scenario points is the
measurement, not an infrastructure failure. The full model population below is
still the documented release review; the minimum is what blocks a tag.

If the fixed local service is unavailable, one configured cloud profile may
stand in for the minimum and is recorded as such in the release directory and
the release notes; the local population remains the review target. The baseline
artifact records its model and profile, so a later reader can tell which was
measured.

## Cost and behaviour bands (0.9+)

`make bench-release-check` compares the new artifact with the highest earlier
release directory under `bench/results/releases/` for the same `(model,
profile)`; `BENCH_PREVIOUS_DIR` overrides the discovery. On the scenarios
successful in both artifacts (the common successful corpus, floor 30):

- prompt+completion tokens: the median of the repeat-3 totals per scenario,
  aggregated across the common corpus, within +10%;
- tool calls: aggregated across the common corpus, within +10%;
- compaction failures: zero;
- hard stop / wall-budget failures: zero;
- per-scenario wall: at most `max(3x, +60 s)`; TTFT at most
  `max(3x, +10 s)` — a pathology ceiling, not a micro-regression;
- model score: within 5.0 of the previous artifact on the same model/profile.
  When the corpus changed (0.9 adds scenarios the 0.8.9 artifact did not run),
  the top `model_score` is a different weighted aggregate and is reported but
  not gated; the band is applied to the mean per-scenario score of the
  scenarios present in both artifacts, scaled to the 0..1000 axis.

The per-scenario repeat variance is why tokens are compared in aggregate, and
the wall/TTFT ceilings exist to catch a hang rather than a slow tail. The score
band is a ratchet, not a significance test (the 0.8.9 95% CI is ±11.3 points; a
larger drop is a release decision recorded in the artifact README, not a silent
pass). The gate is `tools/bench_release_check.py`; its tests are
`tests/gates/test_bench_release_check.py`. A deliberate override uses
`--cost-band` / `--comparable-floor` and is recorded in the release notes.

The 0.8.9 artifact is the 0.9 baseline: the self-comparison is 37 common
successful scenarios, 3,550,162 tokens and 210 tool calls. The 0.8.9 -> 0.8.8
comparison was green (tokens -1.8%, calls -10.2%) before the 0.9 run.

## Pending validations (re-scoped while the service is down)

The session brief, the 32 KB tool-result budget on a live long read, and
plan/task default-on each need a live model. Their procedures are:

1. Brief: a compression run with and without the brief; compare redundant calls
   and post-compression continuation. The switch gates only the injection —
   compression still extracts the brief — so the two runs differ in exactly what
   reaches the model:
   ```sh
   AMBER_SESSION_BRIEF=0 ./amber-bench run --live --profile <profile> \
     --scenario k-01-compression-stress --repeat 3 --out brief-off.json
   AMBER_SESSION_BRIEF=1 ./amber-bench run --live --profile <profile> \
     --scenario k-01-compression-stress --repeat 3 --out brief-on.json
   ```
2. Budget: the long-read scenario with `AMBER_MAX_TOOL_RESULT_BYTES=0` vs
   `32768`; compare last-request bytes, tokens, steps and success.
3. Default-on plan/task: the same engine with `--disable tool_plan,tool_task`
   vs the default; compare score, tool calls, redundant calls and failures.

They are not claimed as run. The local service is inviolate and was inactive;
hermetic evidence is the only measurement until it is confirmed up.

## What runs

1. **Harness floor (hermetic, ~2 min):**
   ```
   ./amber-bench run --cat harness --out bench/results/harness-baseline-vN.json
   ```
   Expected: integrity 1.0 (42/42 probes). Any dip = harness regression —
   the release is BLOCKED until green. Commit the baseline.

2. **Model population (live, ~5-6 h, serial, single GPU slot):**
   ```
   ./bench/run_all_models.sh
   ```
   - Uses the llama-turboq **in-process** swap registry (`models.ini`
     aliases + `POST /models/load`) — no restart, no config edit, no
     parallel server.
   - Strictly serial: one model → full 49-scenario run → next. The runner
     forces `subagent_parallel=false` (single-slot constraint).
   - Writes `bench/results/<alias>-bench.json` + `<alias>-scorecard.txt`
     per model, restores `qwopus` at the end (unconditional trap).

3. **Per-model scorecards (already written by the script):**
   ```
   ./amber-bench scorecard bench/results/<alias>-bench.json
   ```

## Publishing (the chart inputs)

1. Commit all `bench/results/*-bench.json` + `*-scorecard.txt`.
2. Update the **Model population table** in `BENCHMARK.md`:
   ranking by score, pass count, loop-live mean, wasted, redundant.
   Regenerate the table from the JSONs:
   `python3 - <<EOF ... (see BENCHMARK.md section) ... EOF`
3. Add a **release delta** line: vs previous release — model score deltas
   per model, harness integrity, and the top-3 failure classes (see below).
4. The chart is the BENCHMARK.md table; the scorecards are the per-model
   drill-down.

## Reading the results (top failure classes, as of 2026-08-13)

Population of 10 models, 112 total failures:

| Class | Share | Meaning |
|---|---|---|
| Genuine model failures | 41% | scenario checks not met (efficiency, adherence, wording) |
| Tool-choice (bash over `search`) | 21% | models reach for bash grep instead of the search tool |
| Over-criticism (clean-code controls) | 17% | models invent issues in clean code (arch-02/clean-02) |
| Scenario-authoring artifacts | 7% | check case-sensitivity (e.g. "Hash Set" vs "set") |
| Wall-clock budget | 7% | run exceeds the wall budget (engine-enforced since v0.3.1) |
| Oracle over-strict | 5% | h-02 write-vs-bash append oracle strictness |

**Universal (10/10 models):** l-plan-design, arch-02, clean-02. **Common
(8/10):** l-tool-choice, ds-01.

## Rules

- **Service inviolate:** only the documented `/models/load` swap. Never
  restart/edit the service; never spawn a parallel server. Restore
  `qwopus` at the end (the script's trap does this unconditionally).
- **Serial only:** single GPU slot; no parallel inference requests
  (prefill penalty). The script and the runner enforce this.
- **No mid-run edits:** rebuild the bench binary BEFORE launching; a
  mid-run binary change makes the run invalid for comparison.
- If a model fails to load (OOM at the preset ctx), record it as a finding
  and continue — do not reconfigure the service.

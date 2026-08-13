# Release Benchmark Procedure

**Purpose:** every release, run the full model population through the harness
benchmark and update the benchmark chart. This is the win/lose/stagnate
signal for the release: harness health, per-model ranking, and the top
failure classes.

## When

At every release (tag-driven, `vX.Y.Z`) — after the release branch is cut
and before merge to main. Target: run within a few hours, single GPU.

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

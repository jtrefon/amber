# Spec: Harness KPI Catalog — the complete measurement surface

Status: **proposal — awaiting approval**
Companion: `benchmark/harness.md` (probe architecture), `benchmark/MISSION.md`
(why). This document defines **what** we measure and **why it is actionable**;
`harness.md` defines the probe mechanics.

## Principle

Every KPI must answer one of three questions — **winning, losing, or
stagnating** — and must point at a specific harness component when it moves.
A KPI without a named fix path is dead weight. Each entry below carries:
definition, unit, source (recorder/runner/probe/template), determinism axis
(hermetic engine / live model / both), the probe that exercises it, and the
actionable signal.

## 1. Fidelity — the engine must not corrupt anything passing through it

| KPI | Unit | Definition | Axis | Probe | Actionable signal |
|---|---|---|---|---|---|
| `parse_fidelity` | 0..1 | SSE deltas → message fields (content, tool_calls, reasoning) reconstructed bit-exact | hermetic | parse_tool_calls_roundtrip, parse_reasoning_segmentation | a red probe names the exact field corrupted |
| `extraction_precision` | 0..1 | extracted calls that were genuinely tool calls / all extracted | hermetic | extract_no_false_positive | prose JSON wrongly promoted → over-aggressive extractor |
| `extraction_recall` | 0..1 | genuine tool calls extracted / all sent (bare JSON, XML, `<tools>`, attr style) | hermetic | extract_bare_json, extract_tool_call_xml, extract_tools_wrapper, extract_attribute_style, extract_multiple_calls | a model's calling style silently dropped → recall gap (the 32B bug class) |
| `usage_fidelity` | 0..1 | prompt/completion tokens parsed from the final chunk match sent values | hermetic | parse (usage stats) | wrong token accounting skews cost/latency KPIs |
| `truncation_fidelity` | bool | >64 KiB tool output capped with marker, no crash | hermetic | output_truncation | missing marker → the model silently sees partial data |

## 2. Dispatch fidelity — calls execute exactly as specified

| KPI | Unit | Definition | Axis | Probe | Actionable signal |
|---|---|---|---|---|---|
| `dispatch_success_rate` | 0..1 | approved+executed calls / requested calls | hermetic + live | dispatch_roundtrip | a silent drop → dispatch path lost a call |
| `arg_value_fidelity` | 0..1 | tool receives the exact argument values (value-level, not key-level) | hermetic | fidelity_params_value | value substitution → arg round-trip bug |
| `wire_shape_compat` | bool | arguments as JSON object AND as JSON string both dispatch | hermetic | fidelity_arg_shapes | one server dialect breaks all calls |
| `unknown_tool_grace` | bool | unknown tool → denial envelope, loop continues, no crash | hermetic | fidelity_unknown_tool | hard stop on unknown tool → brittle dispatch |
| `parallel_correctness` | bool | multi-call message → all execute, results pair to the right call | hermetic | (new) dispatch_parallel | wrong pairing → cross-call contamination |
| `out_of_order_results` | bool | results completing out of order still pair correctly | hermetic | (new) dispatch_out_of_order | swapped results → corrupted reasoning |
| `result_roundtrip` | bool | result sealed in context with correct role/name | hermetic + live | dispatch_roundtrip | missing/wrong context entry → agent can't see its own results |

## 3. Context integrity — the conversation is never corrupted

| KPI | Unit | Definition | Axis | Probe | Actionable signal |
|---|---|---|---|---|---|
| `chain_integrity` | bool | FNV hash chain survives push/pop/clear/rebuild | hermetic | context_chain_survives | assert in debug → any in-place mutation is caught |
| `compression_preservation` | bool | compression rebuild preserves message content and chain | hermetic | (new) context_compression | compression drops/alters messages silently |
| `token_count_fidelity` | 0..1 | `token_count()` matches content after rebuild | hermetic | (new) context_tokens | budget decisions based on wrong tokens |

## 4. Loop control — the engine must terminate and break out correctly

| KPI | Unit | Definition | Axis | Probe | Actionable signal |
|---|---|---|---|---|---|
| `breakout_latency` | turns | identical tool call repeated → detection fires ≤3 | hermetic + live | loop_infinite_breakout | latency > 3 → detector too slow |
| `detector_precision` | 0..1 | legitimate distinct calls NOT flagged / all distinct calls | hermetic | loop_no_false_positive | false positives → detector kills real work |
| `text_loop_steer_latency` | turns | repeated text → steer injected at 2× | hermetic | loop_text_repeat | steer never fires → model freewheels |
| `fail_streak_recovery` | bool | 3 failing calls → recovery steer; escalation → honest stop | hermetic | loop_fail_streak | no steer → repeated failures burn budget |
| `hard_stop_honesty` | bool | runaway → hard_stop recorded, failure flagged, score capped | hermetic + live | loop_hard_stop_honesty | silent runaway → the loop never terminates |
| `done_signal_handling` | bool | "done" terminates with no extra dispatch, no steer noise | hermetic | loop_done_flag | extra dispatch after done → termination contract broken |
| `continue_signal` | bool | tool calls keep arriving → NO premature termination | hermetic | loop_continue_flag | early stop → harness gives up mid-task |
| `budget_steps` | bool | never exceeds max_steps | hermetic + live | budget_max_steps_enforced, loop_hard_stop_honesty | budget breach → iteration cap regression |
| `budget_wall` | bool | never exceeds max_wall_ms | hermetic | (new) budget_wall_clock | wall cap ignored → runaway cost |

## 5. Agentic planning — the model designs steps; the harness measures them

| KPI | Unit | Definition | Axis | Probe | Actionable signal |
|---|---|---|---|---|---|
| `plan_design_quality` | 0..1 | executed tool mix vs declared `optimal_plan` (plan_ratio) | live | loop_plan_design | ratio ≪ 1 → model can't design an efficient plan |
| `plan_adherence_ratio` | 0..1 | oracle steps matched in dependency order / total | live | loop_plan_adherence | drops → model abandons its own plan |
| `replan_adapted` | bool | a failure is followed by a DIFFERENT call (adaptation) | live | loop_replan_adapt | always repeats → stagnation, not adaptation |
| `dependency_violation_rate` | 0..1 | ordered steps matched out of order / total steps | live | loop_dependency_order | write-before-read → planning without dependencies |
| `tool_economy` | count | wasted + redundant + off-oracle calls per scenario | live | (measured in every live scenario) | high waste → model surveys instead of executing |

## 6. Tool execution quality — the model uses tools correctly

| KPI | Unit | Definition | Axis | Probe | Actionable signal |
|---|---|---|---|---|---|
| `tool_choice_precision` | 0..1 | oracle tool matched / total calls (bullseye) | live | fidelity_misuse_wrong_tool | bash-cat instead of read → tool-choice weakness |
| `arg_precision_value` | 0..1 | value-level param fidelity on on-oracle calls | live | fidelity_params_value | wrong paths/limits → model doesn't read its own args |
| `output_interpretation` | bool | the next call reflects the previous tool's output | live | output_acts_on_content | read X then ignore X → interpretation gap |
| `forbidden_tool_rate` | count | forbidden tools called per scenario | live | (scenario checks) | policy ignored → adherence problem |

## 7. Recovery & robustness — the harness compensates for failures

| KPI | Unit | Definition | Axis | Probe | Actionable signal |
|---|---|---|---|---|---|
| `retry_success` | bool | retryable error retried, run completes | hermetic | recovery_retryable_recovers | no retry → fragile transport handling |
| `retry_nonretryable` | bool | non-retryable error does NOT loop forever | hermetic | (new) recovery_nonretryable | infinite retry → backoff logic broken |
| `dropout_recovery` | bool | mid-stream dropout → retry and complete | hermetic | (new) recovery_dropout | no recovery → flaky networks kill runs |
| `4xx_repair` | bool | request-side 4xx repaired once (drop tools / model swap) then retried | hermetic | (new) recovery_4xx | no repair → dead-end on model mismatch |
| `steer_effectiveness` | bool | received a steer AND completed | hermetic + live | loop probes + KPI | steer fires but model ignores → steer wording weak |
| `recovery_path_distribution` | map | repaired / steer / model recovery counts per run | hermetic + live | (recorder status parse) | one path dominates → recovery monoculture |
| `envelope_fidelity` | bool | error text + meta + status survive the result envelope | hermetic | output_envelope_ext, envelope_status_classification | stripped envelope → model can't diagnose |

## 8. Resource & performance — the engine's cost profile

| KPI | Unit | Definition | Axis | Source |
|---|---|---|---|---|
| `ttft_ms` | ms | time to first token | live | recorder stats |
| `tps_avg` | tok/s | completion tokens / generation time | live | recorder stats |
| `wall_ms` | ms | full run wall time | both | runner |
| `peak_rss_kb` | KiB | peak process memory | both | resource meter |
| `cpu_ms` | ms | user+sys CPU | both | resource meter |
| `tokens` | count | prompt+completion per run | live | recorder stats |
| `files_touched` | count | write-tool files modified | live | recorder |

## 9. Diagnostics & telemetry — the post-mortem layer

| KPI | Unit | Definition | Axis | Actionable signal |
|---|---|---|---|---|
| `tool_details` | list | per-call {name, args, status, error, denied, timeout, duration_ms} | both | the stored trace, not a count |
| `failure_taxonomy` | map | tool_failures split by error/timeout/denied/unknown/malformed | both | dominant failure reason names the fix |
| `calls_per_step` | dist | mean/p95/max calls per LLM turn | both | p95 ≫ mean → call bursting |
| `breakout_latency` | turns | steps until loop detection fired (0 = none) | both | trend across runs |
| `steer_effective` | bool | steer present AND success | both | trend across runs |

## Test inventory — what exists vs what must be added

### Hermetic (engine mechanics) — 34 probes exist
parse 2, extract 6, dispatch 1, context 1, recovery 1, envelope 1, budget 1,
confinement 1, oracle 1, loop 11, fidelity 5, output 3.

### To add (all hermetic unless noted)
1. `dispatch_parallel` — multi-call message, all execute, correct pairing
2. `dispatch_out_of_order` — out-of-order completion, correct pairing
3. `context_compression` — compression rebuild preserves content + chain
4. `context_tokens` — token count fidelity after rebuild
5. `budget_wall_clock` — max_wall_ms enforced
6. `recovery_nonretryable` — no infinite retry loop
7. `recovery_dropout` — mid-stream dropout recovers
8. `recovery_4xx` — request-side 4xx repaired once, retried
9. `extract_prose_no_false_positive` (recall on mixed prose) — partially covered

### Live (model behavior) — new `loop-live` suite
1. `l-plan-design` — multi-file task; measure plan_ratio vs optimal
2. `l-plan-adherence` — task that invites skipping steps; measure adherence_ratio
3. `l-loop-repeat` — task that invites repeating a call; measure repeats,
   breakout (with detection on), and whether the harness must intervene
4. `l-loop-termination` — task where the model should stop after done; measure
   premature-done vs correct-done
5. `l-tool-choice` — task solvable by read vs bash cat; measure choice precision
6. `l-output-interpretation` — read a config then apply it; measure whether the
   next action reflects the content

## Reporting

The `--cat harness` report renders: per-family passed/total, per-probe
detail/expected, harness_integrity. The live model report renders every KPI
above per scenario + per suite. A `harness report` delta view compares two
records (pre/post change) and flags any KPI that moved beyond noise —
that is the win/lose/stagnate read.

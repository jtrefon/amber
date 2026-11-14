# Spec: Hermetic benchmark corpus and publishing

Status: **proposal — awaiting approval on the spec PR.**

Tracking: #596 (epic) and #597–#601 (children).

Companions: `benchmark/MISSION.md` (purpose and definitions of done),
`benchmark/corpus.md` (scenario taxonomy), `benchmark/harness.md` (probe axis),
`benchmark/kpi-framework.md` (runner and scoring), `benchmark/kpi-catalog.md`
(the indicators).

## 1. Why this exists

The benchmark mission already states the contract:

> **Deterministic hermetic suite = harness truth.** The fake-LLM suite runs the
> engine with a scripted model: deterministic, network-free, CI-safe.
> Model-independent by construction — every hermetic failure is the harness's
> failure. This is the regression gate.

The implementation is half of that. On `origin/main` at 2026-11-13:

| metric | value |
|---|---:|
| scenario files | 58 |
| with a hermetic `fake_replies` script | 8 |
| skipped by `amber-bench run` in fake-LLM mode | 50 |
| scenarios compared by `make bench-check` in CI | 8 |

`bench/runner.cpp` skips a scenario with no script:

```cpp
if (!opts.live && s.fake_replies.empty())
    continue;
```

So `tools`, `terminal`, `prompt`, `coding`, `refactor` and every `review-*`
suite have prompts and oracles but never run per PR. The CI gate ratchets only
the eight recovery, compression and delegation scenarios. `MISSION.md`'s
definitions of done — "the hermetic suite is fully green in CI" and "no
dimension below 5 scenarios" — do not hold.

This spec defines the missing half: the corpus, the runner extensions, the
report, the gate, the publishing surfaces and the binary driver. It does not
add a second benchmark harness and does not change the engine.

## 2. Coverage

### Before #597 (the gap)

The eight hermetic scenarios were the recovery, compression and delegation
cases. Everything else was live-only.

| suite | scenarios | hermetic | live-only |
|---|---:|---:|---:|
| agent-failures | 6 | 6 | 0 |
| compression | 1 | 1 | 0 |
| delegate | 1 | 1 | 0 |
| tools | 6 | 0 | 6 |
| terminal | 6 | 0 | 6 |
| prompt | 5 | 0 | 5 |
| coding | 5 | 0 | 5 |
| refactor | 4 | 0 | 4 |
| review-arch | 2 | 0 | 2 |
| review-clean | 2 | 0 | 2 |
| review-datastructures | 2 | 0 | 2 |
| review-patterns | 2 | 0 | 2 |
| review-quality | 2 | 0 | 2 |
| review-refactor | 2 | 0 | 2 |
| repo | 2 | 0 | 2 |
| shaping | 2 | 0 | 2 |
| headroom | 2 | 0 | 2 |
| loop-live | 6 | 0 | 6 |
| **total** | **58** | **8** | **50** |

### After #597

The deterministic suites now carry a scripted plan. The 18 remaining live-only
scenarios are the ones that need a live model or a real repository:
`headroom`, `loop-live`, `repo`, `shaping`, and the
`review-datastructures`/`patterns`/`quality` suites.

| suite | scenarios | hermetic | live-only |
|---|---:|---:|---:|
| agent-failures | 6 | 6 | 0 |
| coding | 5 | 5 | 0 |
| compression | 1 | 1 | 0 |
| delegate | 1 | 1 | 0 |
| headroom | 2 | 0 | 2 |
| loop-live | 6 | 0 | 6 |
| prompt | 5 | 5 | 0 |
| refactor | 4 | 4 | 0 |
| repo | 2 | 0 | 2 |
| review-arch | 2 | 2 | 0 |
| review-clean | 2 | 2 | 0 |
| review-datastructures | 2 | 0 | 2 |
| review-patterns | 2 | 0 | 2 |
| review-quality | 2 | 0 | 2 |
| review-refactor | 2 | 2 | 0 |
| shaping | 2 | 0 | 2 |
| terminal | 6 | 6 | 0 |
| tools | 6 | 6 | 0 |
| **total** | **58** | **40** | **18** |

`corpus.md` already labels `tools`, `prompt` and `coding` as "hermetic + live"
or "hermetic with fake". This spec is the implementation of that intent, not a
new direction.

## 3. Goals and non-goals

### Goals

- A hermetic corpus large enough to be a per-PR gate: at least 30 scenarios
  across at least 6 dimensions.
- Multi-turn scenarios, so follow-ups and context carry are measured.
- A bounded environment matrix, so a failure says *where* the harness fell, not
  just that it did.
- One merged report with both the scenario KPIs and the probe scorecard.
- A durable, published hermetic baseline per release, rendered on the website.
- A binary driver that proves the installed `amber-cli` boots, runs and exits
  with the same tool sequence as the in-process harness.

### Non-goals

- No new harness: this extends `bench/`.
- No engine changes: `bench/` stays an observer over `AgentHooks`.
- No UI rendering benchmark: the ncurses layer is covered by the pty suite.
- No live-model ranking changes: the live release gate is unchanged.
- No per-scenario issues: scenarios live as a table in #597.
- No dynamic badge or per-PR trend database in v1.

## 4. Scenario schema

All additions are optional and additive. A scenario that uses none of them
behaves exactly as it does today.

### 4.1 Hermetic authoring (`fake_replies`)

A hermetic scenario supplies a fixed plan: one scripted assistant reply per
step, then a final answer. The oracle still scores what the harness executed;
the script is not a transcript of the oracle.

Rules:

- one scripted reply per oracle step, then a final assistant message;
- a scenario that tests recovery keeps the failing reply in the script (a
  retryable error, a mid-stream dropout, an empty reply, a denied tool);
- no network, no live model, no absolute host paths, no timestamps, no random
  ids;
- `latency_ms` is fixed by the scenario, not measured;
- a scenario that scripts a side-effecting tool declares `"approval": "allow"`;
  the default keeps the deny-unless-process policy so the denied-tool scenario
  still exercises the refusal path;
- `platforms` still gates the scenario to the hosts that can run it.

### 4.2 Multi-turn (`turns[]`)

```json
"turns": [
  {
    "prompt": "Read main.cpp and explain it.",
    "fake_replies": [{"tool_calls": [{"function": {"name": "read", "arguments": "{\"path\":\"main.cpp\"}"}}]}, {"content": "It prints hello."}]
  },
  {
    "prompt": "Add error handling and verify it compiles.",
    "fake_replies": [{"tool_calls": [{"function": {"name": "write", "arguments": "..."}}]}, {"tool_calls": [{"function": {"name": "bash", "arguments": "{\"command\":\"g++ -fsyntax-only main.cpp\"}"}}]}, {"content": "done"}],
    "checks": {"must_contain": ["done"]}
  }
]
```

Semantics:

- `prompt` + `fake_replies` + `oracle` + `checks` are turn 0 when `turns` is
  absent.
- All turns run on the same `Agent` and `Context`; one turn is one
  `agent.run()` call.
- The scenario's oracle is the union of its turn oracles; the final checks run
  against the last turn's final answer. Per-turn attribution is diagnostic.
- A turn with no replies, or an empty `turns` array, is a validation error.

### 4.3 Environment variants (`variants`)

```json
"variants": {
  "stream": [false, true],
  "plugins": ["default", "minimal"]
}
```

- `amber-bench run --matrix` runs the cross-product; `--variant key=value`
  runs one cell.
- The product is capped at 8 cells per scenario; more is a validation error.
- Axes are declared by the harness, not free-form. The first set:
  `stream`, `plugins` (`default`/`minimal`), `context_window`, `approval`
  (`deny`/`allow`), `workspace` (`clean`/`dirty`).
- A cell is a bounded environment, not an arbitrary permutation.

### 4.4 Validation

The loader rejects, with the scenario name and the field:

- an empty `fake_replies` when `hermetic_only` is true;
- `turns` with no turns or a turn with no replies;
- unknown variant axes and a cell product above the cap;
- an oracle step naming a tool the scenario's plugin set does not provide;
- a platform the host cannot run (skipped, not failed, as today).

### 4.5 Determinism

The compared fields are deterministic: tool names and arguments, tool status,
token counts, step counts, success and failures. Wall time is reported and has
a pathology ceiling, but is never compared as a precise number. The report must
normalise the temporary workspace path and must not include timestamps in any
field the gate compares.

## 5. Runner semantics

- A scenario gets a fresh workspace, agent, context and tool registry, as
  today.
- Multi-turn runs reuse one `Agent` for the scenario; the context grows by the
  expected messages after each turn.
- Variants expand before the run; each cell gets a fresh workspace and agent,
  and the report names the cell.
- The recorder observes the same `AgentHooks` as today. No engine change is
  needed.
- Hermetic runs write no user state: the workspace is a temp directory and the
  XDG roots are isolated.

## 6. Report schema

The report stays additive; `schema_version` is added when the first new field
lands.

```json
{
  "schema_version": 2,
  "run_id": "...",
  "mode": "hermetic",
  "engine_version": "1.0.0",
  "timestamp": "...",
  "corpus": {"scenarios": 38, "suites": 6, "hermetic": 38},
  "plugins": [{"id": "custom", "enabled": true, "tier": "bundled"}],
  "harness": {"integrity": 1.0, "passed": 42, "total": 42, "families": {}},
  "scenarios": [
    {
      "name": "t-06-write-verify-loop",
      "suite": "tools",
      "success": true,
      "prompt_tokens": 120,
      "completion_tokens": 30,
      "tool_calls_total": 2,
      "wall_ms": 18,
      "turns": [
        {"index": 0, "prompt": "...", "prompt_tokens": 60, "completion_tokens": 15, "tool_calls": 1, "success": true, "failures": []}
      ],
      "variant": {"stream": false, "plugins": "default"},
      "failures": []
    }
  ],
  "suite_summary": [
    {"suite": "tools", "scenarios": 6, "passed": 6, "prompt_tokens": 900, "completion_tokens": 200, "tool_calls": 14, "wall_ms": 120}
  ]
}
```

- `mode: hermetic` is required for the published harness artifact.
- The headline is `harness.integrity` and the scenario pass count, **not**
  `model_score`. A hermetic run may still carry `model_score` for schema
  compatibility, but no published hermetic surface uses it as a quality score.
- `suite_summary` is derived from `scenarios`; it is not separately authored.
- `baseline` is added by the gate, not by the run: `{path, violations, added,
  missing}`.

## 7. Gate contract

`make bench-check` runs the hermetic corpus and compares it with
`bench/results/harness-efficiency-baseline.json`.

- **Every hermetic scenario is gated.** A scenario in the report but not in the
  baseline is *additive* only while it passes; an additive scenario with
  `success: false` fails. A baseline scenario missing from the report fails
  closed.
- A new scenario is admitted to the baseline only by `make bench-baseline`,
  deliberately, with the diff reviewed.
- Thresholds stay: tokens above +10% fail, tool calls above +10% fail, a
  baseline success that becomes a failure fails, wall time above
  `2x + 500ms` fails.
- The corpus floor is at least 30 hermetic scenarios and at least 6 suites;
  dropping below either fails closed.
- A suite that disappears from the report fails, even if its scenarios are
  individually accounted for.
- The gate fails closed on a missing or malformed report, an empty scenario
  list, or a report whose `mode` is not `hermetic`.

## 8. Publishing contract

Three surfaces, one source of truth.

### Per pull request

- The `bench-efficiency` job runs `make bench-check`.
- The run page carries a per-suite table: pass/total, tokens delta, calls
  delta and probe integrity.
- The JSON report is uploaded as the `bench-efficiency` artifact.
- No commit and no baseline rewrite from CI. A regression can be allowed for
  one PR with `bench-allow: <reason>`, as today.

### Per release

- The release PR commits `bench/results/hermetic/<version>.json` next to
  `bench/results/releases/<version>/` (the live artifact).
- The GitHub release links both: the live model+harness score and the
  hermetic harness pass rate.
- The release checklist gains one line: the hermetic artifact exists and its
  `engine_version` matches the tag.

### Website

- `bench/results/hermetic/<version>.json` is the canonical data.
- `make website-bench-data` copies the latest committed file into
  `website/src/data/hermetic.json`; the Astro build reads that file and never
  reaches outside the website root.
- The benchmarks page has two separate sections: **Hermetic harness**
  (pass rate, probes, tokens, calls) and **Live model** (the 0–1000 score).
  They never share an axis.
- A stale hardcoded table is removed; the page is regenerated from the
  committed file.

## 9. Binary driver and telemetry

### `amber-cli --json-out <path>`

One JSON summary at exit, written even when the run fails:

```json
{
  "schema_version": 1,
  "exit_code": 0,
  "engine_version": "1.0.0",
  "turns": [
    {
      "prompt": "...",
      "reply": "...",
      "tool_calls": [{"name": "read", "args": {"path": "main.cpp"}, "status": "ok", "ms": 3}],
      "usage": {"prompt_tokens": 100, "completion_tokens": 20, "cache_read_tokens": 0},
      "wall_ms": 12
    }
  ],
  "usage": {"prompt_tokens": 100, "completion_tokens": 20, "cache_read_tokens": 0},
  "wall_ms": 120,
  "plugins": ["custom"],
  "workspace": "/tmp/..."
}
```

Streaming stays on stdout; the file is the telemetry surface. The exit code is
unchanged.

### Mock server

- A loopback HTTP server in `bench/`, ephemeral port.
- Serves `/v1/models` and `/v1/chat/completions`, stream and non-stream.
- Scripts tool calls, content, usage and retryable/non-retryable errors.
- Serialises requests and records them for divergence checks.
- No external network.

### Driver

- `amber-bench run --driver binary` spawns `amber-cli` with isolated
  `XDG_CONFIG_HOME`, `XDG_STATE_HOME` and `XDG_CACHE_HOME`, and a temp
  workspace.
- It compares the telemetry with the in-process run: exit code, tool sequence,
  per-tool status and (where deterministic) usage.
- At least 5 scenarios: boot + read, search + write, multi-turn, denied tool,
  plugin tool.
- A `bench-e2e` job gates `ci-gate` in the `cpp` dimension, bounded and
  skipped for docs-only diffs.
- A missing packaged prompt or data file fails the driver; that is the point.

## 10. Test plan

Every child starts with a red test.

| Area | Test |
|---|---|
| Scenario schema | `bench_test`: `turns[]`, `variants`, invalid cases |
| Multi-turn runner | `bench_test`: one Agent, context grows, per-turn report |
| Variant expansion | `bench_test`: cross-product, single cell, cap, unknown axis |
| Report schema | `bench_test`: merged `--cat both --out`, `schema_version` |
| Gate | `tests/gates/test_bench_check.py`: additive failure, missing, floor |
| Publishing | report-schema test plus a website build that reads the data |
| Telemetry | `bench_test`: `--json-out` on success and failure |
| Mock server | `bench_test`: stream/non-stream, errors, request log |
| Driver | `bench_test`: tool-sequence match, XDG sentinel, missing file |

## 11. Rollout

| PR | Scope | Depends on |
|---|---|---|
| 1 | #597 corpus and additive-gate fix | — |
| 2 | #598 multi-turn scenarios | #597 |
| 3 | #599 environment variants | #597 |
| 4 | #600 merged report and publishing | #597 |
| 5 | #601 binary driver and telemetry | #600 |

The epic should land before #563 (microkernel split) so the refactor has a
pre-refactor baseline. The harness can also measure the `v1.0.0` tag from a
later branch; the 1.0 release does not have to wait for it.

## 12. Decisions and open questions

Decided here:

- The hermetic headline is pass rate plus probe integrity, not a 0–1000
  score.
- The corpus is the existing `bench/scenarios` tree; no second corpus.
- The website reads a committed file through a generated
  `website/src/data/hermetic.json`.
- The binary driver uses `amber-cli`, not a new TUI one-shot mode.
- Multi-turn runs in one process; restart persistence is a separate scenario.

Open for review:

- Which two variant axes ship first: `stream` + `plugins`, or `stream` +
  `context_window`?
- Is the binary driver a gate from its first PR, or advisory for one release?
  The plan is a gate for the five scenarios, with an explicit override if it
  proves flaky.
- Does the first merged report bump `schema_version` to 2, or does it keep 1
  until a field is removed? The plan is 2, because `turns` and `variant` are
  structural additions.

## 13. Acceptance

The epic's exit criteria are the acceptance test: at least 30 hermetic
scenarios across at least 6 dimensions, every one gated; multi-turn and
variants measured; one merged report; a committed hermetic release artifact;
the website rendering it; and the binary driver agreeing with the in-process
tool sequence.

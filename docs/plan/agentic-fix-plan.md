# Agentic Fix Plan — P1..P5 implementation guide (self-contained)

Status: approved for implementation (strategy: `docs/plan/agentic-architecture-review.md`)
This document gives a fresh session everything needed to implement all five
items. Follow the repo's TDD workflow for every item: RED test commit →
green implementation → lint/analyze clean → **before/after live benchmark run**
recorded in `BENCHMARK.md`. One change at a time — never two items in one PR.

---

## 0. Session context (read first)

### Repo map (relevant parts)

| Path | Purpose |
|---|---|
| `lib/agent.cpp`, `lib/agent_helpers.cpp`, `lib/dispatch.cpp` | the agent loop; `format_tool_envelope` in agent_helpers.cpp:33 |
| `include/agent/tool.h` | `Tool` port + `ToolResult{ok, output, error, meta}` |
| `include/agent/tools.h`, `lib/tools_default.cpp` | tool factories + `register_default_tools(reg, jobs, cancel)` |
| `include/agent/job.h` | `JobService` — the host-owned-state pattern to copy for P1 |
| `include/agent/compressor.h`, `lib/compressor.cpp`, `lib/compressor_request.cpp` | compression gate + classify prompt (P5) |
| `tools/` | tool adapters: read, write, search, bash, process_* |
| `bench/` | `amber-bench` harness: scenario loader, runner, recorder, kpi, report |
| `bench/scenarios/*.json` | the corpus (31 scenarios, 9 templates) |
| `prompts/system.md`, `tools.md`, `skills.md`, `mcp.md`, `git.md` | runtime prompts (descriptive philosophy — no forcing language) |
| `BENCHMARK.md`, `bench/results/` | published KPI records + raw JSONs (before/after ground truth) |
| `tests/run_tests.cpp`, `tests/agent_loop_test.cpp`, `tests/bench_test.cpp` | test suites (TDD home) |
| `tests/fake_llm.h` | hermetic FakeLLMClient pattern |

### Commands

```sh
make                    # builds libagent_core.a, libagent_tools.a, amber, amber-cli, amber-bench
make test               # run_tests + agent_loop + e2e + completions + plugin + ws + bench_test
make lint               # clang-tidy (MUST be clean before commit)
make analyze            # cppcheck (MUST be clean before commit)
./amber-bench run --suite agent-failures          # hermetic corpus (no model needed)
./amber-bench run --live --profile qwen35-dense --out .amber/bench/results/x.json
./amber-bench report bench/results/*.json --format markdown   # regenerate BENCHMARK.md body
```

Live benchmark setup: local models via the systemd llama server hot-reload
(`POST /models/load {"model":"<preset>"}` — never start a second server);
cloud models via env:
`AMBER_API_BASE=https://api.kilo.ai/api/gateway AMBER_API_KEY=<key>`
(key lives in `~/.config/amber/providers/kilocode.conf`).
`--debug DIR` writes per-scenario wire + conversation logs.

### Workflow rules

1. **RED first**: write the failing test, commit it (CI shows red), then GREEN.
2. **One change per PR**, with a before/after benchmark row in `BENCHMARK.md`.
3. Baseline models for proof: **Qwen3.6-27B dense** (local, preset `qwen35-dense`)
   and **Poolside Laguna S 2.1** (cloud, `poolside/laguna-s-2.1:free`).
4. Style: `.clang-format` (LLVM, 4-space, 100 cols), no comments restating code,
   no SPDX headers, methods ≤10 lines, classes ≤200 lines.
5. Prompts: descriptive only — "never"/"don't"/"must"/"do not" are banned from
   `prompts/` (repo convention, AGENTS.md).
6. After touching headers: `make clean && make` (stale `.d` files cause ABI bugs).

### Baseline numbers (2026-08-04, prompt v2)

| Model | score | pass | agentic | plan% | tools | redundant | failures |
|---|---|---|---|---|---|---|---|
| Qwen3.6-27B dense | 913 | 24/25 | 71 | 69 | 104 | 9 | 5 |
| qwopus-27b | 906 | 24/25 | 66 | 62 | 105 | 13 | 4 |
| Laguna S 2.1 (free) | 887 | 23/25 | 66 | 60 | 117 | 4 | 6 |
| Nemotron 550B (free) | 821 | 21/25 | 54 | 54 | 142 | 17 | 18 |

Records: `bench/results/*.json` (baseline) + `bench/results/prompt-v2/*.json` (prompt v2).

---

## 1. P1 — Plan tool (externalized task tracking)

**Hypothesis (H1)**: models re-read files and repeat calls because task state
lives only in context. An external, model-maintained task list (opencode
`TodoWrite`, Claude Code `TodoWrite`) gives working memory that survives
compression and reduces redundant work.

### Architecture

- `include/agent/plan.h` — `struct PlanItem {std::string id, text; bool done;}` +
  `class PlanStore` (host-owned, like `JobService`):
  ```cpp
  class PlanStore {
  public:
      std::string add(const std::string& text);        // returns id
      bool update(const std::string& id, const std::string& text);
      bool complete(const std::string& id);
      bool remove(const std::string& id);              // optional, for cleanup
      const std::vector<PlanItem>& items() const noexcept;
      void clear() noexcept;
  };
  ```
  IDs: `p1`, `p2`, … (monotonic). No filesystem, no threads, no persistence.
- `tools/plan_tool.cpp` — `std::unique_ptr<Tool> make_plan_tool(PlanStore&)`:
  - `name()`: `"plan"`; `requires_approval`/`is_read_only`: false.
  - `parameters_schema()`: `{"op": {"enum": ["add","update","complete","list","clear"]}, "id": string, "text": string}` (op required; id/text as needed by op).
  - `execute()`: performs the op, returns the **current plan list** compactly:
    `p1 [x] fix parsing\np2 [ ] write tests` (`[x]` = done). Never throws; errors as `ToolResult{false, ...}`.
- Registration: `register_default_tools` gains `PlanStore& plan` parameter
  (breaking signature — update all callers: `src/main.cpp`,
  `tui/tui_main.cpp`, `bench/runner.cpp`, tests that call it).
- No loop changes. The envelope applies to plan results like any tool.

### Spec (contract)

- The plan tool is **advisory working memory**: the agent decides when to use
  it. Nothing enforces it (philosophy: empowerment, not confinement).
- `plan.list` after compression must return the full pre-compression state —
  this is the tool's core value (state survives compression because it lives
  in the store, not context).
- Result size cap: 200 items max, each text ≤ 200 chars (enforced in the tool;
  oversized `text` truncates).

### TDD (RED list — `tests/run_tests.cpp` + `tests/agent_loop_test.cpp`)

1. `plan_store_add_update_complete_list` — PlanStore semantics incl. id
   monotonicity and update of missing id → false.
2. `plan_tool_ops` — tool add/list/complete via `execute()`; result text
   contains ids and `[x]` markers; unknown op → error ToolResult.
3. `plan_tool_registered_by_default` — registry contains `plan` after
   `register_default_tools`.
4. `agent_loop_plan_survives_turns` (hermetic, FakeLLMClient): scripted model
   calls `plan.add` in turn 1 and `plan.list` in turn 2; assert the second
   result contains the item from turn 1 (state persists across the loop).
5. Bench: new scenario `planning-multi-file` (suite `tools`, d5, live):
   setup 4 small files (`a.c`..`d.c` each with a distinct `// FIX n` marker),
   prompt: "fix all four markers, verify each file compiles" —
   oracle: 4× `write` (unordered) + 1× `bash` compile; optimal_plan
   `{"plan":1,"write":4,"bash":1}`; expected_steps 8. Measure plan-tool usage
   via tool_calls in the report (not enforced in oracle).

### Benchmark gate (acceptance)

On Qwen3.6-27B dense + Laguna S 2.1: `redundant` ↓, `steps` ↓, `plan%` ↑ vs
the baseline rows. Record new rows in `BENCHMARK.md` (new section
"P1: plan tool — before/after").

---

## 2. P2 — Lean tool results (drop the args echo)

**Hypothesis (H2)**: the envelope's full `args` echo (write calls echo the whole
edits JSON) inflates context per result → faster degradation → re-reading.

### Current format (`lib/agent_helpers.cpp:33`)

```
[tool=<name> args=<full args dump> status=<status> meta=<meta dump>]
<content>
[end]
```

### Target format

```
[tool=<name> status=<status> meta=<meta dump>]
<content>
[end]
```

- Drop **only** the `args=` echo. Keep `status` and `meta` (structured, small,
  used by the loop/UI/recorder for denied/timeout detection and summaries).
- Update the envelope contract in `prompts/tools.md` (header field table:
  remove the `args` row; adjust the intro text describing the shape).
- Keep `format_tool_envelope`'s signature — the args parameter becomes unused;
  remove it from the signature and update the two call sites (agent.cpp
  dispatch + confirm paths) or keep `(void)args` if call sites are many —
  prefer removing the parameter (cleaner; check callers with rg).

### TDD

1. Grep tests for envelope assertions (`format_tool_envelope`,
   `"args="`, `[tool=`): fix any that pin the args echo (RED → update to the
   new format).
2. New hermetic test: `agent_loop_tool_envelope_lean` — after a scripted
   `write` tool call, the tool message in context contains `[tool=write
   status=ok` and does **not** contain `args=`.
3. Recorder/bench tests must stay green (recorder reads status via
   `meta.denied`/`meta.timeout`, unaffected).

### Benchmark gate

Tokens/run ↓ (prompt+completion sums in the JSON records), `steps` ↓,
`redundant` ↓ on both baseline models. Before/after rows in BENCHMARK.md.

---

## 3. P3 — Schema-first tools (trim tools.md prose)

**Hypothesis (H3)**: every tool is documented twice (7 KB+ of prose in
`prompts/tools.md` + full OpenAI schemas); long prompts get skimmed → wrong
args → failures.

### Target

- `prompts/tools.md` keeps: the working-style section (descriptive), the
  **envelope contract** (updated per P2), and a **one-line-per-tool index**
  (name + one sentence). Delete the per-tool parameter tables (they exist in
  the schemas; verify schema descriptions are complete first — read each
  tool's `description()`/`parameters_schema()` in `tools/` and enrich the
  schema descriptions where the prose carried unique info).
- This is a **prompt change** → same cycle as the prompt-v2 work: benchmark
  before/after on both baseline models; the proof loop is the harness itself.

### TDD

1. Grep tests for `tools.md` content assertions (like the skills/mcp prompt
   tests) — update wording pins.
2. No behavior tests needed beyond the benchmark gate (prompts are data).

### Benchmark gate

`tool_failures` ↓ (and no regression in score/pass on baseline models).

---

## 4. P4 — Sub-agents (task tool)

**Hypothesis (H4)**: parallel/delegated exploration with isolated context is a
core competitive mechanism we lack.

### Architecture

- `include/agent/subagent.h` + `lib/subagent.cpp`:
  ```cpp
  struct SubAgentOptions { int max_iterations = 20; size_t max_result_bytes = 16384; };
  // Runs one focused sub-task in a fresh context with the same tool registry,
  // approval policy and workspace; returns the final text (or an error string).
  std::string run_subagent(Config cfg, ToolRegistry& reg, AgentHooks hooks,
                           const std::string& sub_prompt,
                           const SubAgentOptions& opts, std::string& err);
  ```
  Implementation: construct an `Agent` with a fresh `Context` (a fresh Agent
  is exactly that), inject the sub-prompt as the user message, call
  `run()`, return the reply. Compression/memory/skills: same config; memory
  retriever may be omitted in v1 (sub-agent is a focused worker).
- `tools/task_tool.cpp` — `make_task_tool(...)`:
  - `name()`: `"task"`; params: `{"description": string, "prompt": string}`.
  - `execute()`: runs `run_subagent` **synchronously** (v1 — no parallelism),
    returns the sub-agent's final text as the result.
  - Approval: the sub-agent inherits the parent's `on_approval` (same policy
    store); `requires_approval`: false (it is a delegation, not a side effect).
- **Hook isolation**: the sub-agent must use hooks that do NOT leak tool
  calls/status into the parent's recorder/UI. Pass a silent-ish hook set
  (`on_status` only) into `run_subagent`; the parent's recorder sees exactly
  one `task` call. Bench accounting: nested calls are excluded from parent
  KPIs by construction (they never reach the parent recorder).
- Budget: hard iteration cap (SubAgentOptions), result size cap; on cap/error
  return a `ToolResult{false, ...}` with what was done.

### TDD (hermetic, FakeLLMClient)

1. `subagent_runs_focused_task` — scripted fake: parent calls `task` with a
   prompt; the nested Agent consumes its own fake replies (tool call → result
   → final text); parent result contains the sub-agent's final text.
   (The fake must be shared/scripted for both agents — order matters; the
   nested agent runs to completion before the parent continues.)
2. `subagent_hooks_do_not_leak` — parent recorder sees exactly one `task`
   call; no nested tool calls appear.
3. `subagent_iteration_cap` — scripted fake that never terminates → sub-agent
   stops at `max_iterations`, parent gets an error result, parent loop
   continues.
4. `task_tool_registered` — registry contains `task`.

### Gate / sequencing

P4 is gated on the **repo-level corpus suite** (multi-file scenarios) — add
first, then P4. New suite `repo` with 2–3 scenarios: a small multi-file bug
(two files + header, hidden test template compiles the pair), a long-horizon
task (5+ steps across files). Sub-agent use is measured (tool_calls) not
enforced. Benchmark gate: `repo` suite scores; parent `tools`/`steps` ↓.

---

## 5. P5 — Compression timing (gate fires far too late)

**Finding (measured)**: `CompressionConfig` defaults are
`threshold=0.50, min_turns=10, cooldown=20` and the gate computes
`budget = context_size>0 ? context_size : 32k`, firing when
`tokens/budget >= 0.50` (`lib/compressor.cpp:46-55`). With the local server
reporting `n_ctx=262144`, the gate fires at **~131k tokens** — effectively
never for normal runs. Context grows unbounded → late-turn degradation →
re-reading.

### Target

- Cap the effective budget: `budget = min(context_size, kMaxCompressBudget)`
  with `kMaxCompressBudget = 32'000` (const in compressor.cpp). The gate then
  fires at ~16k tokens regardless of n_ctx. Keep the threshold configurable.
- Revisit `min_turns=10` / `cooldown=20` only after the budget change is
  measured (one change at a time).
- The classify/extract prompt (`lib/compressor_request.cpp`) already retains
  the active task — no prompt change in P5.

### TDD

1. Hermetic gate test (FakeLLMClient + small `cfg.context_size`):
   `compression_gate_fires_at_capped_budget` — with context_size=262144 and
   scripted turns exceeding 16k tokens, compression fires (assert
   `Agent::should_compress()` / compress invoked via observer).
2. Bench scenario `k-01-compression-stress` (hermetic, suite `compression`,
   d4): scripted fake with many turns + large token counts; final answer must
   still be correct **after** a compression mid-run; assert the compressed
   context still contains the active task facts (checks on final answer).
3. Existing compression tests in `run_tests.cpp` must stay green (they pin the
   current threshold math — update expectations to the capped budget).

### Benchmark gate

`k-01` passes; steps/reads on long scenarios ↓ (less late-run degradation).

---

## 6a. P1 status (2026-08-04)

Shipped: `todowrite` tool + `TodoStore` (full-list replacement, informed by
opencode's actual TodoWrite source — the model sends the whole list, the
store replaces it; no op-based API). Hermetic tests green, zero-warning
build, no benchmark regression (qwen dense 913 → 911, within variance).
**Adoption: 0** across all experiments (wiring / complexity / advertisement
hypotheses all disproven with wire-level evidence — verdict: model training
fit, H5). **Now feature-flagged: `plan_tool` config key / `AMBER_PLAN_TOOL`
env, default OFF** — off means no registration, no schema, no prompt
section (zero per-request cost); on registers the tool + appends
`prompts/tools_planning.md`. The tool stays off until a model trained on
the TodoWrite convention arrives or P4 lands. Reordered sequencing:

| Order | Item | Note |
|---|---|---|
| 1 | **repo-level suite** (was 4th) | now the critical path — gates P1 proof AND P4 |
| 2 | P2 lean envelope | independent, do next |
| 3 | P3 schema-first tools | independent |
| 4 | P4 task tool | builds on repo suite + P1 store pattern |
| 5 | P5 compression budget | independent |

## 6b. P2 status (2026-08-04, final: P2v2)

**P2 (full args drop) — abandoned after 3 runs**: failures gain was noise
(18→10/5/17, reverted), and redundant rose consistently (+5, 17→22/23/20 —
a real side effect: the args echo was the model's confirmation).

**P2v2 (conditional echo, shipped)**: args echoed when compact (≤120 chars),
dropped for large payloads. Two A/B runs on Nemotron 550B free (25 shared
scenarios, same prompt):

| metric | before | v2 r1 | v2 r2 | mean Δ |
|---|---|---|---|---|
| score | 82.1 | 84.7 | 83.6 | +2.0 |
| tools | 142 | 123 | 135 | −9% |
| steps | 164 | 148 | 159 | −6% |
| failures | 18 | 8 | 13 | down both runs |
| redundant | 17 | 16 | 19 | baseline |

Verdict: consistent modest gains, no side effect — the redundant uptick of
P2 is proven to be lost confirmation. Statistically suggestive (n=2), not
sealed; re-confirm on the local baseline when inference returns. Hermetic
tests: `agent_loop_tool_envelope_lean` (large payload not echoed),
`agent_loop_tool_envelope_small_args_echoed` (small args echoed).
Next: P3 (schema-first tools).

## 6d. P4 status (2026-08-05, MECHANISM SHIPPED, flag-gated — adoption is framing-dependent)

Sub-agents built and shipped: `task` tool + `SubAgentExecutor` with
runtime-configurable serial/parallel execution (`/set subagent parallel|max`,
`amber.conf` `subagent_parallel`/`subagent_max`, env overrides). Serial mode
runs workers one at a time — sequential requests share the prompt prefix,
keeping provider prompt caches warm (DeepSeek cached hits ~90% discounted;
parallel requests with distinct prefixes pay full price each). Parallel mode
runs concurrently under the max cap. Tests: focused task, iteration cap,
serial (peak concurrency 1), parallel (peak 2), nesting guard (async
dispatch workers inherit the sub-agent state; exactly one launch), recorder
isolation, config keys, completions tree, hermetic d-01 scenario.

Two live A/B runs on the full corpus (Nemotron 550B free):

| metric | P2v2 mean | P4 r1 | P4 r2 | mean Δ |
|---|---|---|---|---|
| score | 85.1 | 82.3 | 81.7 | −3.1 |
| pass | 24.5 | 24 | 23 | −1.0 |
| tools | 172 | 178 | 173 | +3.5 |
| redundant | 24 | 27 | 27 | +3.0 |
| failures | 15.5 | 18 | 18 | +2.5 |
| steps | 181.5 | 186 | 183 | +3.0 |

**task usage: 0 calls in every repo scenario** — same adoption wall as P1.

Verdict: default-on costs ~3 points of schema noise and buys nothing
unprompted. **The tool is now flag-gated (`task_tool`, default off)** like
plan_tool; the executor + serial/parallel config stay live and apply
whenever the tool is enabled. A delegation-shaped scenario
(`d-01-two-area-survey`, suite `delegate`) invites splitting two
independent surveys into task calls; live adoption when invited: **2/3 runs
delegated (bullseye 1.0, 0 wasted steps)** vs 0% unprompted. Conclusion:
the mechanism works; usage follows task framing, not availability.
Records: `bench/results/prompt-v2/nemotron-550b-p4*.json`, `-d01.json`.

## 6. Sequencing, acceptance, PRs

| Order | Item | PR title prefix | Acceptance |
|---|---|---|---|
| 1 | P1 plan tool | `feat: plan tool — externalized task tracking` | tests green + before/after rows: redundant↓, steps↓ |
| 2 | P2 lean envelope | `feat: lean tool results — drop args echo` | tests green + tokens/run↓ |
| 3 | P3 schema-first tools | `feat: schema-first tools — trim tools.md prose` | tests green + failures↓ |
| 4 | repo suite first, then P4 | `feat: repo-level benchmark suite` → `feat: task tool (sub-agents)` | suite green + repo scores |
| 5 | P5 compression budget | `feat: cap compression gate budget at 32k` | tests green + k-01 passes |

Each PR: RED commit → green commit → `make clean && make && make test &&
make lint && make analyze` all green → live before/after run on both baseline
models → `BENCHMARK.md` section with the new rows → push → PR. Do not merge
into main without the benchmark proof (the harness is the gate).

## 7. Risks & non-goals

- **P4 recursion**: task-inside-task is possible; v1 caps nesting at 1
  (the sub-agent's registry also has `task` — either remove it from the
  sub-agent's registry or document the cap; prefer removing in v1).
- **P2 format change** may briefly confuse models trained on verbose results;
  the benchmark will show it. Keep `status`/`meta` (needed by the loop).
- **P3**: don't strip the envelope contract or working style — only the
  per-tool parameter tables.
- Non-goals: no interface MUST/NEVER tone (philosophy stays), no approval
  gate changes, no engine telemetry additions (the recorder already covers
  measurement).

# Spec: Benchmark & KPI Framework (architecture)

Status: **Full-scope proposal — awaiting approval**
Owner: engine team
Depends on: `AgentHooks`, `LLMClientFactory`, `ConversationLog`, `ToolRegistry` —
all existing ports. **Zero engine changes required.**

Mission: `benchmark/MISSION.md` (why the benchmark exists, what it measures, what
"strong" means — the reference and baseline).
Companion specs: `benchmark/corpus.md` (scenario taxonomy), `benchmark/kpi-catalog.md`
(measurable indicators).

## Why

Goal: the fastest, most accurate, most robust agentic engine — bullseye tool
selection, least steps to solution, strongest recovery, smallest footprint.
The framework turns engine behavior into objective, repeatable KPIs:

1. **Regression-gate** the engine hermeticly (fake LLM, deterministic, CI-safe).
2. **Compare models** locally against a free endpoint (good vs poor, same corpus).
3. **Quantify prompt strength**: is the right tool advertised and chosen at the
   right moment; does the agent hold its system prompt?
4. **Score artifacts** (coding/refactoring tasks) against deployed static
   templates — hidden tests + structural checks, no human judgment.
5. **Ship with the app**: `amber-bench` becomes the tool we (and the other
   project we port to) use to prove engine quality.

## Why NOT a plugin / NOT in-engine

- **Plugin** (`lib/plugin.cpp`): external processes with exactly 3 JSON-RPC
  methods (`initialize`, `tool.call`, `shutdown`); no reverse RPC; **no
  agent-loop observation** (no hooks, no context access); TUI-only wiring.
  Plugins are capability servers, not observers — every KPI we need lives
  inside `Agent` behind hooks.
- **In-engine**: the engine is the thing being measured. Built-in telemetry
  corrupts SRP and the benchmark's trustworthiness. `AgentHooks`
  (agent.h:54-78) exists precisely so observers (TUI, CLI, bench) stay outside.
- The benchmark is a **third thin client** linking `libagent_core.a` +
  `libagent_tools.a`, sibling to `amber-cli`. If a future KPI needs a
  structured signal the hooks cannot carry, that is a separate, spec'd engine
  change — not this framework's concern.

## Layout

```
bench/
  scenario.h/.cpp      scenario model, loader, validator
  recorder.h/.cpp      BenchmarkHooks : AgentHooks → event stream
  oracle.h/.cpp        tool-call oracle matcher (ordered/unordered steps, wildcards)
  kpi.h/.cpp           KPI aggregation per scenario + per run
  resources.h/.cpp     process resource sampling (RSS, CPU, baseline)
  template.h/.cpp      static-template engine (compile, hidden tests, structure checks)
  runner.h/.cpp        workspace setup → Agent build → run → teardown
  report.h/.cpp        renderers: text table, JSON, markdown; result history
  profiles.h/.cpp      model profiles (good / poor / custom)
  fake.h/.cpp          benchmark FakeLLMClient (hermetic; extends tests/fake_llm.h pattern)
  main.cpp             amber-bench CLI
  scenarios/**         scenario JSON + template dirs (checked in)
  profiles.json        model profile presets
tests/bench_test.cpp   hermetic unit tests (oracle, recorder, KPI math, loader, template engine)
```

`bench/` joins `LINT_SRCS` and `make analyze`; `tests/bench_test.cpp` joins
`UNITTEST_OBJ`; `amber-bench` is a new target like `cli`/`tui` (binaries in
repo root, installed by `make install`).

## CLI surface

```
amber-bench list [--suite S]
amber-bench run [--suite S] [--scenario NAME] [--profile P] [--repeat N]
               [--live] [--template-cache DIR] [--out .amber/bench]
amber-bench validate-template <scenario>     # prove hidden tests pass on reference
amber-bench report [--run ID] [--format text|json|markdown]
```

- Default (no `--live`): hermetic — scripted fake LLM, deterministic, no model
  needed. This mode is safe for CI.
- `--live`: real model from `amber.conf`/flags/env (same config layering as
  `src/main.cpp`), against any OpenAI-compatible endpoint (free local model).
- `--profile poor` etc.: override model/temperature/thinking per
  `bench/profiles.json` for cross-model runs.
- `--repeat N`: statistical runs (live mode); aggregates report median/p95.

## Scenario schema (full)

```jsonc
{
  "name": "multi-step-read-modify-write",
  "suite": "tools",
  "description": "find the bug, patch it, verify",
  "platforms": ["linux", "darwin"],        // runner skips unsupported hosts
  "hermetic_only": false,                   // true → never run live
  "model_profiles": ["good", "poor"],       // default: all profiles
  "setup": {
    "files": {"src/main.c": "int main(){ return 1; }"},
    "shell": ["git init"]
  },
  "prompt": "the app exits with code 1; fix it and show the diff",
  "oracle": [                               // ordered by default
    {"tool": "read", "args": {"path": "*"}, "args_subset": true},
    {"tool": "write"},
    {"tool": "bash", "args": {"command": "*git*diff*"}, "unordered": true}
  ],
  "forbidden_tools": ["process_start"],     // use → prompt-adherence penalty
  "prompt_checks": {                        // system-prompt adherence (assistant text)
    "must_contain": ["done"],
    "must_not_contain": ["I can't"]
  },
  "checks": {                               // final-answer assertions
    "must_contain": ["return 0"],
    "must_match": "regex:^ok.*"
  },
  "template": "coding/sorting",             // static-template engine (see below)
  "budget": {"max_steps": 10, "max_wall_ms": 120000}
}
```

Matcher rules:

- Oracle steps match in order; `unordered: true` steps form a set matched in
  any order (parallel dispatch). Wildcards: `*` in args; `args_subset` relaxes
  to key-subset matching; absent `args` matches any arguments.
- `forbidden_tools` calls and off-oracle calls count as wasted calls, not
  failures (unless budget is exceeded).
- Success = oracle fully matched && all checks && no budget breach && no
  hard stop.

## Runner lifecycle (per scenario)

1. `Workspace::set_root()` to a fresh temp dir; materialize `setup.files`,
   run `setup.shell` (platform-gated).
2. Build `Config` (hermetic: stream off, canned context size; live: layered
   config), `ToolRegistry` + `register_default_tools`, compression gate,
   compressor, memory store, retriever — identical to `src/main.cpp:191-271`.
3. Snapshot resource baseline (`getrusage` + RSS before run).
4. Construct `Agent` with `BenchmarkHooks` recorder + (hermetic) fake client or
   (live) `HttpLLMClient` via `LLMClientFactory` (agent.h:49).
5. `run(prompt)` with wall-clock + per-phase timing around it; recorder
   collects the event stream (tool pairs, stats, state transitions, status
   strings parsed for retry/recovery/steer signals).
6. If `template`: run the static-template engine on the agent's artifact.
7. Score → KPI record; append to history; teardown workspace.

## Static-template engine (`template.h`)

Objective scoring for coding/refactoring tasks — the "deployed static
template" guarantee. Each template dir:

```
bench/scenarios/coding/sorting/
  TASK.md            task + constraints (shown as prompt basis)
  skeleton/          starting code the agent edits
  reference/         golden solution (harness-only, never shown to agent)
  hidden_tests/      compile-and-run test suite (fixtures + asserts)
  checks.json        structural checks:
                     {"must_contain": ["class SortingAlgorithms"],
                      "must_not_contain": ["std::sort("],
                      "compile_with": ["g++", "clang++"]}
```

Engine steps:

1. **Validate** (`amber-bench validate-template`): reference solution must
   compile and pass 100% of hidden tests → template sound before deployment.
2. **Run**: compile agent's artifact with the declared compiler(s); run hidden
   tests → `artifact_score` (passed/total), `compile_ok`.
3. **Structure checks**: pattern checks on artifact text (must/must-not
   contain); `static_analysis_findings` runs cppcheck/clang-tidy on the
   artifact with a fixed config when available.
4. Refactor scenarios add **behavior-equivalence**: the exact same hidden-test
   run must pass on reference and artifact (outputs byte-identical) — proves
   the refactor preserved behavior.

Templates are checked in and platform-gated (`compile_with`), so hidden tests
run on linux and macOS with g++/clang++/clang.

## Hermetic fake LLM (P1 extension of the `tests/fake_llm.h` pattern)

The bench fake adds to the existing script surface:

- per-reply latency (for step-time KPIs),
- streaming delta sequences + **mid-stream dropout** (throw after N chunks —
  server-dropout scenarios),
- scripted retryable/non-retryable errors (already in the test fake),
- token stats per reply.

Deterministic: same script → same event stream → same KPIs (wall-clock KPIs
are still measured, but assertions use the scripted values).

## Report & history

- Per scenario: KPI record (see `kpi-catalog.md`).
- Per run: suite/profile aggregates (median, p95 where `--repeat` > 1),
  model_robustness matrix when multiple profiles ran.
- History: `.amber/bench/results/<run-id>.json` + JSONL event stream per
  scenario (tool pairs, stats, state transitions) for post-mortem.
- Renderers: text table (default), JSON (machine), markdown (release notes).

## Phasing

- **P1** (first PR): harness skeleton (scenario loader, recorder, oracle, KPI
  math, resources, report text/JSON) + hermetic fake + suites seeded with a
  few scenarios each (see `corpus.md` for the P1 list) + `tests/bench_test.cpp`
  (oracle/matcher, recorder parsing, KPI math, loader validation, template
  engine compile-path, one end-to-end hermetic scenario) + `amber-bench`
  binary + lint/analyze + `make test` wiring.
- **P2**: live-model comparison workflows + trends across history + `/bench`
  TUI command (reuses report lib) + MCP stub server suite + skills suite +
  compression-stress suite + `--repeat` statistics.
- **P3**: release-note benchmarking workflow, scenario-pack distribution
  (community corpus), optional semantic answer-similarity scoring.

## Non-goals

- No engine modification (no new hooks, no in-loop telemetry).
- No CI gate with real models (free-model constraint); hermetic unit tests are
  the CI gate.
- No human-judgment metrics (SOLID/KISS/DRY/YAGNI adherence is measured by
  objective proxies only — see `kpi-catalog.md` §Judgment metrics).
- Disk I/O is not a KPI (see `kpi-catalog.md` — no signal for an LLM agent).

## Cross-references

- Ports used: `AgentHooks` (agent.h:54-78), `LLMClientFactory` (agent.h:49-50),
  `ContextEventSource` (context.h:148-163), `Stats` (llm.h:43-49),
  `ConversationLog` (config.h:101).
- Fake pattern to extend: `tests/fake_llm.h`; client construction blueprint:
  `src/main.cpp:191-271`.
- Corpus: `benchmark/corpus.md`. Indicators: `benchmark/kpi-catalog.md`.

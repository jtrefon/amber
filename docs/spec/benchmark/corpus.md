# Spec: Benchmark Corpus (scenario taxonomy)

Status: **implemented (#628); the deterministic layers are gated.**
Companion: `benchmark/kpi-framework.md` (harness), `benchmark/kpi-catalog.md`
(KPIs), `benchmark/hermetic-corpus.md` (hermetic corpus and publishing).

Every scenario is a declarative JSON + optional static-template dir, scored by
the harness — **no human judgment at scoring time**. Scenarios start a few per
suite and grow over time; each suite covers one dimension of engine quality.


Mission: `benchmark/MISSION.md` — the benchmark's purpose, dimensions map, and
model-library strategy.

## Suites

Status: implemented (#628). The corpus has **22 suites / 86 scenario files**;
68 carry a deterministic script, policy or trace and 18 are live-only
(`headroom`, `loop-live`, `repo`, `shaping` and the
`review-datastructures`/`patterns`/`quality` suites). The deterministic layers
are specified in `benchmark/agent-simulation.md`, `benchmark/trace-replay.md`
and `../tui/e2e-testing.md`; the gates print their own counts.

```sh
find bench/scenarios -mindepth 2 -maxdepth 2 -name '*.json' | wc -l  # 86
make bench-check        # the hermetic cells; the baseline + additive counts
make agent-e2e-check    # 19 CLI journeys against the committed baseline
make long-horizon-check # 5 repository journeys
```

| Suite | Measures | Mode | Files |
|-------|----------|------|------:|
| `agent-failures` | robustness against typical agent failure modes | deterministic | 6 |
| `terminal` | real shell work with predefined outcomes | deterministic | 6 |
| `tools` | the right tool for the job | deterministic | 6 |
| `prompt` | system-prompt adherence | deterministic | 5 |
| `multi-turn` | follow-ups and context carry across user turns | deterministic (turn scripts) | 3 |
| `coding` | algorithm/interview tasks via static templates | deterministic | 5 |
| `refactor` | design quality via behavior-equivalence + structure checks | deterministic | 4 |
| `compression` | long-context correctness under the compression gate | deterministic | 1 |
| `delegate` | sub-agent delegation and owner reports | deterministic | 1 |
| `policy` | policy-zoo discrimination on the `zoo` scenario | deterministic | 2 |
| `e2e` | CLI journeys: approval, cancel, session, subagent, skill, MCP, plugin, provider, compression | deterministic + real binary | 18 |
| `repo-long` | multi-module repository changes with hidden tests | deterministic | 5 |
| `review-arch`, `review-clean`, `review-refactor` | quality perception controls | deterministic | 2 each |
| `review-datastructures`, `review-patterns`, `review-quality` | quality perception tiers | live-only | 2 each |
| `headroom`, `loop-live`, `repo`, `shaping` | live headroom/loop/repository/shaping | live-only | 2 / 6 / 2 / 2 |

Platform rule: every scenario declares `platforms` (`linux`, `darwin`, or
both). The runner skips unsupported hosts; terminal scenarios use commands
present on both.

The per-suite tables below name the families and their intent; the current file
set is `bench/scenarios/<suite>`, and the count in the table above is the
source of truth.

The skills, MCP, plugin and provider surfaces are covered by the `e2e`
journeys (`e2e-skill-*`, `e2e-mcp-*`, `e2e-plugin-*`,
`e2e-provider-*`) and the TUI journeys rather than separate P2 suites; the
scenario workspace supplies the skill/MCP/plugin fixtures.

---

## `agent-failures` — robustness against typical agent failure modes

| # | Scenario | What it exercises | Key KPIs |
|---|----------|-------------------|----------|
| f-01 | `redundant-call-elimination` | Prompt asks for a file listing; agent must NOT call the same read twice. Oracle: exact reads once; second identical read counts as wasted. | wasted_calls, bullseye_ratio |
| f-02 | `mistake-then-recover` | Agent calls read on a wrong path (oracle allows the miss), gets `ok:false`, must correct and land on the right path. | recovery_count, steps_to_solution, success |
| f-03 | `mid-stream-dropout` | Hermetic: fake LLM dies mid-stream (after N chunks); agent must retry and complete. Live: model endpoint flakiness recorded. | retry_rate, wall_time, success |
| f-04 | `parallel-vs-sequential` | Oracle with `unordered` steps (parallel dispatch correctness) AND a scenario where order matters (unordered not allowed) — catches wrong serialization assumptions. | bullseye_ratio, arg_precision |
| f-05 | `hallucinated-args` | Agent cites a file that does not exist; verify-after-action adherence required. | prompt adherence, wasted_calls |
| f-06 | `empty-reply-recovery` | Hermetic: model returns empty content → agent must self-correct (empty-turn fallback). | recovery_count, success |
| f-07 | `tool-denied-grace` | Bash tool denied by approval gate (hermetic: Deny); agent must continue with allowed tools, not abort. | success, wasted_calls |

## `terminal` — predefined-outcome shell tasks (linux + darwin)

Predefined outcome = the scenario's `checks` assert the exact observable result
(exit code, file content, count), so correctness is objective.

| # | Scenario | Task | Outcome assertion |
|---|----------|------|-------------------|
| t-01 | `ls-count-files` | Count files matching a pattern in a fixture tree | exact count in final answer |
| t-02 | `grep-extract-value` | Extract a value from a fixture file (config/ini/log) | exact value + source line |
| t-03 | `compile-run-cpp` | Write + compile a tiny C++ program with g++/clang++, run it | exit code 0 + exact stdout |
| t-04 | `env-inventory` | Gather OS/user/cwd/toolchain facts | expected subset present, no invented facts |
| t-05 | `pipeline-transform` | Chain commands: sort/filter/join fixture data to a file | output file byte-identical to golden (template) |

## `tools` — right tool for the job

| # | Scenario | Prompt strength question | Oracle |
|---|----------|--------------------------|--------|
| t-01 | `search-vs-bash-grep` | Does the agent use `search` (advertised for this) instead of `bash grep`? | oracle allows only search |
| t-02 | `read-vs-cat` | Use `read` (paginated, confined) not `bash cat` | read only |
| t-03 | `write-vs-rewrite` | Patch-style `write` for a small edit, not full rewrite via bash heredoc | write only |
| t-04 | `process-vs-blocking-bash` | Long-running task → `process_start` rather than a blocking bash call that would time out | process_start, then tasks |
| t-05 | `tools-dropped-4xx` | Hermetic: server rejects tool schema → adapter drops tools → agent still completes via plain text | success + tool_counts sequence |

## `prompt` — system-prompt adherence

| # | Scenario | Measured behavior |
|---|----------|-------------------|
| p-01 | `envelope-format` | Final message follows the mandated framing (done-marker, no invented tool output) |
| p-02 | `banned-tool-refusal` | `forbidden_tools` declared; agent must refuse to use them even when convenient |
| p-03 | `verify-after-action` | After a write, oracle expects a read-back/verification step before "done" |
| p-04 | `no-hallucinated-facts` | Final answer must not contain facts absent from workspace (checks against fixture content) |

## `coding` — algorithm/interview via static templates

Each scenario ships `template/` (TASK.md, skeleton, reference, hidden_tests,
checks.json) and is scored by the template engine — compile + hidden tests +
structure checks. Starting set (grows with the corpus):

| # | Scenario | Task | Hidden tests assert |
|---|----------|------|---------------------|
| c-01 | `fizzbuzz` | trivial warmup; skeleton provided | exact output for n=1..100 |
| c-02 | `sorting-multi` | implement 3 sorts (no `std::sort` — structure check) | outputs + stability on fixtures |
| c-03 | `ring-buffer` | data-structure task: fixed-capacity queue | push/pop/overwrite semantics, edge cases |
| c-04 | `lcs` | string DP algorithm | exact lengths + reconstruction |
| c-05 | `graph-bfs` | adjacency → distances | distances on fixtures, cycle handling |

## `refactor` — design quality via behavior-equivalence + structure checks

Behavior-equivalence = the same hidden tests pass on reference and on the
agent's artifact with byte-identical outputs — the refactor must change
*shape*, not *behavior*. Structure checks are pattern-based and objective
(`must_not_contain` a discriminator switch, `must_contain` an interface).

| # | Scenario | Task | Checks |
|---|----------|------|--------|
| r-01 | `extract-method` | Split a monster function into named helpers | must_contain helper names; behavior-equivalence |
| r-02 | `polymorphism-over-switch` | Replace type-dispatch switch/if-else with virtuals | must_not_contain `switch (type)`; must_contain virtual base |
| r-03 | `adapter-pattern` | Adapt a legacy interface to the required contract | must_contain adapter class; behavior-equivalence |
| r-04 | `proxy-pattern` | Add a caching proxy in front of a slow service | must_contain proxy; call-count hidden tests |
| r-05 | `complexity-reduction` | Reduce cyclomatic complexity of a nested branch maze | `static_analysis_findings` (cppcheck/clang-tidy on artifact) + behavior-equivalence |
| r-06 | `interface-decoupling` | Replace concrete dependency with an abstraction (DIP) | must_contain interface; hidden tests inject a fake impl |

## Reproducing the counts

`find bench/scenarios -mindepth 2 -maxdepth 2 -name '*.json'` counts scenario
files (template `checks.json` and the long-horizon template dirs are not
scenarios). `make bench-check` prints the hermetic cell count and its additive
cells; `make agent-e2e-check` prints the CLI journey count; `make
long-horizon-check` prints the repository journey count; `make trace-check`
prints the trace corpus test count; `make policy-matrix-check` prints the zoo
scenario count. A healthy change updates the **gate baselines**, not this table;
the table is the taxonomy, and the commands above are the source of truth.

Growth rule: new scenarios must score on objective oracles/templates only —
if a metric needs human judgment, it does not get a scenario until a template
makes it objective. A new deterministic scenario is additive in `bench-check`;
a new CLI journey is red until the baseline is regenerated after review.

## Historical P1 scope

The original P1 scope seeded `agent-failures` (f-01..f-07), `terminal`
(t-01..t-06), `tools` (t-01..t-06), `prompt` (p-01..p-06), `coding` (c-01..c-05)
and `refactor` (r-01..r-06). Those counts are history; the table above is the
current corpus.

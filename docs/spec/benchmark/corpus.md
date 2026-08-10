# Spec: Benchmark Corpus (scenario taxonomy)

Status: **Full-scope proposal — awaiting approval**
Companion: `benchmark/kpi-framework.md` (harness), `benchmark/kpi-catalog.md` (KPIs).

Every scenario is a declarative JSON + optional static-template dir, scored by
the harness — **no human judgment at scoring time**. Scenarios start a few per
suite and grow over time; each suite covers one dimension of engine quality.


Mission: `benchmark/MISSION.md` — the benchmark's purpose, dimensions map, and
model-library strategy.

## Suites

| Suite | Measures | Mode | Starter (P1) |
|-------|----------|------|--------------|
| `agent-failures` | Robustness vs typical agent failure modes: redundant calls, mistakes, dropouts, repeated executions | hermetic + live | 4 |
| `terminal` | Real shell work with predefined outcomes; cross-platform (linux + darwin) commands | live (hermetic where scriptable) | 3 |
| `tools` | Right tool for the job — prompt strength for advertised tools | hermetic + live | 4 |
| `prompt` | System-prompt adherence: envelope format, banned-tool refusal, verify-after-action | hermetic + live | 3 |
| `coding` | Algorithm/interview tasks via static templates (hidden tests) | live (template engine; hermetic with fake) | 2 |
| `refactor` | Design-quality tasks: polymorphism, decoupling, adapter/proxy, complexity reduction — behavior-equivalence + structure checks | live (template engine) | 2 |
| `skills` | Skills catalog selection + step adherence | live | 2 (P2) |
| `mcp` | MCP stub server: tool args, tool correctness | live (stub infra) | 2 (P2) |
| `compression` | Long-context correctness under the compression gate | hermetic | 2 (P2) |

Platform rule: every scenario declares `platforms` (`linux`, `darwin`, or
both). Runner skips unsupported hosts — terminal scenarios must only use
commands present on both OSes (POSIX coreutils + `g++`/`clang++` where noted);
OS-specific facts are expressed as expectations, not hardcoded outputs.

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
| t-04 | `process-vs-blocking-bash` | Long-running task → `process_start` rather than a blocking bash call that would time out | process_start, then process_read |
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

## `skills`, `mcp`, `compression` (P2)

- `skills`: s-01 catalog selection (right skill picked), s-02 step adherence
  (skill's steps followed in order). Needs the skills environment set up in
  the scenario workspace.
- `mcp`: m-01 stub-server tool called with correct args, m-02 stub failure
  surfaces as tool error and agent recovers. Needs a stub MCP server binary in
  the test harness (P2 infra).
- `compression`: k-01 correctness survives the compression gate mid-task,
  k-02 compression does not fire too early (cooldown respected). Hermetic only
  (needs scripted context growth).

## P1 scope (first PR)

Suites seeded: `agent-failures` (f-01, f-02, f-03, f-04, f-06, f-07),
`terminal` (t-01..t-06), `tools` (t-01..t-04, t-06), `prompt` (p-01..p-03,
p-05, p-06), `coding` (c-01..c-05: fizzbuzz, sorting, ring-buffer, lcs, bfs),
`refactor` (r-01..r-04: extract-method, polymorphism, adapter, strategy).
31 scenarios + 9 template dirs, each with difficulty (1-5) and expected_steps.

Growth rule: new scenarios must score on objective oracles/templates only —
if a metric needs human judgment, it does not get a scenario until a template
makes it objective.

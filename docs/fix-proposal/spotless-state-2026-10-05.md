# Spotless state — proposal

Status: **proposal, awaiting sign-off.** Nothing here has been implemented.

"Spotless" needs a definition before it can be a target, so this proposes one and then
measures the tree against it. The headline: **you are much closer than the docs claim,
and what remains is one real axis plus one wrong line of documentation.**

## Definition of spotless

Every measured axis at or under its cap, with an **empty baseline** — so the gate is a
cliff, not a ratchet, and the next contributor cannot reintroduce what is left.

## Measured state (2712 owned C++ functions)

| axis | cap | max | at/over cap | baseline | verdict |
|---|---|---|---|---|---|
| cyclomatic (CCN) | 14 | **13** | 0 | empty | spotless |
| NLOC | 40 | **39** | 0 | empty | spotless |
| nesting | 4 | ≤4 | 0 | empty | spotless |
| class size | 200 | <200 | 0 | empty | spotless |
| clang-tidy | — | — | 0 | empty | spotless |
| TODO/FIXME/HACK | 0 | 0 | 0 | n/a | spotless |
| duplication | — | — | 0 | n/a | spotless |
| **params** | **6** | **11** | **18** | **12 groups** | **not spotless** |

Five of eight axes are already at zero. Six are cliffs.

**A correction to my previous message:** I reported "2 functions at CCN 14". Both are
`bench/scenarios/*/hidden_tests/test_main.cpp` — oracle tests that the benchmark *agent*
writes, excluded from the gate by design. Owned code maxes at **13**. That axis is done.

## The one real axis: 18 functions over the param cap

`PARAM_MAX = 6`; 18 functions exceed it, worst at 11. All 18 sit in a ratchet baseline,
so none can get wider, but the baseline is what stands between you and a cliff.

These are not cosmetic. The worst offender is `Agent::Agent` at 11 dependencies:

```cpp
Agent::Agent(Config cfg, ToolRegistry& registry, AgentHooks hooks,
             std::unique_ptr<CompressionStrategy> compressor,
             std::unique_ptr<CompressionGate> gate,
             std::shared_ptr<MemoryStore> memory_store,
             std::unique_ptr<MemoryRetriever> retriever,
             std::unique_ptr<LLMClient> client, LLMClientFactory client_factory,
             bool register_skills, std::shared_ptr<SkillCatalog> skills)
```

Eleven collaborators on one constructor is a dependency-wiring smell, and it matches the
repo's own DIP guidance ("wiring happens at the boundary"). The fix is not to shorten the
list by packing booleans; it is to group the collaborators behind a typed struct or to
construct the client through the factory it already accepts.

**Proposal, in three PRs, one axis per PR:**

1. **`Agent` construction** (`Agent::Agent` 11, `PluginServices::PluginServices` 8)
   Introduce a typed `AgentDeps` aggregate assembled at the call site in `src/main.cpp`
   and `tui/tui_main.cpp`. Both hosts currently spell out all 11; a struct makes the
   boundary explicit and collapses the signature to 2–3. Touches 2 call sites + 1 header.
2. **HTTP / MCP plumbing** (`curl_exec` 11, `spawn_mcp_server` 7, `apply_curl_options` 7)
   The `bool` flag parameters are the problem — `curl_exec`'s 11 include several
   behaviour toggles. Group them into an options struct. Purely mechanical.
3. **Retry loop** (`chat_with_retry_impl` 8, `chat_with_retry` 7, `_strict` 7,
   `dispatch_tool_calls` 9, `prepare_call` 9, `handle_fail_streak` 8)
   A `RetryPolicy` value type. This is the group most likely to find a behaviour bug,
   since the three `chat_with_retry*` variants differ only in policy.

Then `make lint-baseline-update` drops from 12 groups to 0, and PARAM becomes a cliff.

**Each PR follows Red → Green** with a test per behaviour change, per `AGENTS.md`. The
retry-loop PR is the one to review hardest.

## The NLOC axis: recommend leaving it

27 functions at NLOC ≥ 37 against a cap of 40. Getting to an empty baseline means
refactoring 27 more functions, and the evidence says most are not the problem:

```
39 NLOC  CCN  4   bench/probe.cpp  loop_probe_fail_streak
39 NLOC  CCN  5   lib/skill_install.cpp  stage_skill_pack
38 NLOC  CCN  7   lib/plugin.cpp  install
37 NLOC  CCN  4   bench/runner.cpp
```

Flat, linear, 4–5 CCN. Refactoring these would reduce a length measure that has stopped
tracking difficulty, and would churn 27 functions to move a number that no gate threshold
is near. **Recommendation: stop here.** Say so in `AGENTS.md` so the next person does not
re-open it as if it were unfinished.

## A wrong line in `AGENTS.md`

The gate-integrity section states `readability-function-cognitive-complexity` is
ratcheted with **31 known findings**. That is wrong: `tests/lint_baseline.json` is
`{"files": {}}`, and `make lint-baseline-update` reports **0 findings**. The baseline
emptied in #196, not the #182 that introduced the ratchet.

This matters more than a normal doc error, because that section exists so the gates can
be believed. It currently tells you lint is weaker than it is. Fix: correct the count and
cite #196.

## One thing I could not verify

`make lint` cannot run on this machine — clang-tidy is not installed, and the only
reachable pip build (18.1.8) has disagreed with CI's 18.1.3 before. So "0 clang-tidy
findings" is read from the **baseline file**, not a fresh scan. CI's `lint` job is the
real evidence, and it was green on every merged PR, but I am not going to call a number I
did not measure. Match CI with `brew install llvm@18` if you want it locally.

## Work list

| # | work | outcome | risk |
|---|---|---|---|
| 1 | Fix the `AGENTS.md` cognitive-complexity claim | docs truthfulness | none |
| 2 | Record "NLOC burn-down stopped" in `AGENTS.md` | stops re-opening | none |
| 3 | `AgentDeps` aggregate | −2 baseline groups | low, 2 call sites |
| 4 | HTTP/MCP options structs | −3 baseline groups | low, mechanical |
| 5 | `RetryPolicy` value type | −5 baseline groups | **medium**, behaviour-sensitive |
| 6 | `make lint-baseline-update` → 0 | **PARAM becomes a cliff** | gate tightening |
| 7 | Optional: install `llvm@18`, run `make lint` locally | closes the one unverified axis | none |

Items 1–2 are documentation and can land immediately. Items 3–6 are the actual work;
each is its own PR with its own red-green cycle. Item 7 is setup, not a change.

## What I recommend

Do 1 and 2 now — they are one-line corrections and the second prevents re-opening a
finished axis. Then 3, 4, 5 in that order, each reviewed on its own merits, and 6 at the
end to convert the last ratchet into a cliff.

**Do not** shorten signatures by packing booleans into `int` flags or by forwarding a
`void*`. The point of the PARAM axis is readability at the call site, and the 11-argument
constructor is a real design problem that deserves a real fix.
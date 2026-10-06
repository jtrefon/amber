# Sealed pipeline — proposal

Status: **proposal, awaiting sign-off.** Nothing implemented.

You are right on both counts, and I was not paying enough attention.

**On "why is the build green with items outside the cap":** a green build means no
violation *newer* than the baseline. PARAM's 18 known violations are recorded in a
ratchet baseline, so the gate is correct and the headline is still misleading: "7 gates
pass" reads as clean while one axis has accepted debt. The cap is not the state.

**On DIP being a guide:** you are right that it should be a gate, and right that it is
not one. It is prose in `AGENTS.md` with **no enforcement anywhere**. I searched every
gate for it.

## What "sealed" has to mean

Three things, and the first is the one I got wrong:

1. **Every axis has an empty baseline.** A ratchet is not sealed — it is a permanent,
   machine-readable list of accepted violations.
2. **Every axis fails at its cap.** Not "fails when worse than recorded".
3. **The rules being gated are themselves enforced.** DIP is currently a written rule
   with nothing behind it.

## The five gaps, measured

| # | gap | measured |
|---|---|---|
| 1 | **DIP / architecture has no gate** | searched all `tools/*.py`: none. Rule exists only in prose |
| 2 | **PARAM is a ratchet** | 18 violations in a 12-group baseline; cap is 6, worst is 11 |
| 3 | **`include/` ungated** | 64 functions in `include/agent/`, absent from complexity and nesting `ROOTS` |
| 4 | **NLOC headroom is 1** | max 39, cap 40, 27 functions at ≥37 |
| 5 | **Lint unverifiable locally** | clang-tidy absent; 18.1.8 vs CI 18.1.3 have disagreed |

Gap 3 measured *clean*: those 64 header functions have CCN max 7, NLOC max 22, and **0**
over the PARAM cap. So adding `include/` costs nothing today — it closes a hole that
would otherwise be exploited on day one, which is the whole point of sealing.

Gap 4 is not a violation but it is the reason "green" has been reassuring me: with 27
functions inside 3 lines of the cap, the NLOC cliff is one refactor away from being the
thing that breaks, and I reported it as comfortable when it is tight.

## Proposal

### PR 1 — Close the `include/` hole, add a coverage floor for it

Add `"include"` to `complexity_gate.ROOTS` and `nesting_gate.ROOTS`. Both gates already
exclude `third_party` and `include/nlohmann/`, so the vendored header cannot leak in.
Raise `MIN_FUNCTIONS_SANE` to the new measured count so a future ROOTS edit that silently
shrinks coverage fails closed instead of passing.

Effect: 2425 → 2489 functions scanned, 0 new violations. Cheap, and it makes the gate
mean "the whole tree" rather than "the six directories I remembered".

### PR 2 — A dependency-direction gate (DIP as a cliff)

New `tools/arch_gate.py`, wired into `make check` and `ci-gate`. Rules, all mechanically
checkable, none requiring judgement:

- `lib/` and `include/agent/` must not `#include` from `tui/`, `src/`, or `bench/`
- `include/agent/` must not include any third-party header except the ones it already
  uses (`nlohmann/json.hpp`)
- `tools/` must depend on `include/agent/` and never the reverse
- no include cycles between `include/agent/` headers

Each rule fails with the offending file:line. Baseline empty from day one, because the
tree already complies — I will verify that before claiming it.

The deliberate limit: this checks **layering direction**, not whether an abstraction is
the *right* one. A gate that judges design would need the judgement I am arguing against.
Direction is the part that silently rots, and it is fully mechanical.

### PR 3 — Empty the PARAM baseline

Three sub-PRs, each Red → Green, in the order of risk:

1. `AgentDeps` aggregate — `Agent::Agent` 11 → 3. The 11 collaborators are a real DIP
   violation: the core is constructing its own collaborators instead of receiving them.
   This gate would have caught the signature as it grew; it now shrinks it.
2. HTTP/MCP options structs — `curl_exec` 11, `spawn_mcp_server` 7, `apply_curl_options` 7.
   `curl_exec`'s parameter list is mostly `bool` toggles; they belong in an options type.
3. `RetryPolicy` value type — `chat_with_retry{,_impl,_strict}` 7–8,
   `dispatch_tool_calls` 9, `prepare_call` 9, `handle_fail_streak` 8.

Then `make lint-baseline-update` → empty, and PARAM becomes a cliff. **Do not** pack
booleans into `int` flags to pass; the axis measures the call site.

### PR 4 — NLOC headroom

27 functions at ≥37 against a cap of 40. Recommend **lowering `NLOC_MAX` 40 → 36** in one
step: that clears 27 of them and leaves 4 lines of headroom, which is what a cliff needs
to stop being a coin flip. Reaching 36 means ~27 refactors of mostly flat, linear
code at CCN 4–5 — cosmetic work on a measure that has stopped tracking difficulty.

My honest recommendation is the opposite of PR 3's: **do this one only if you want the
axis to stop being the fragile one.** The alternative is to leave 40 and accept that
ordinary edits will trip it.

### PR 5 — Make lint locally reproducible

`brew install llvm@18`, record the version in the developer guide, and make
`make lint` print the analyzer version it used next to its verdict. A gate whose result
cannot be reproduced locally is a gate you can only take on faith between CI runs.

## The green-build question, answered

| axis | cap | worst | baseline | sealed? |
|---|---|---|---|---|
| CCN | 14 | 13 | empty | yes |
| NLOC | 40 | 39 | empty | yes, 1 line headroom |
| nesting | 4 | ≤4 | empty | yes |
| class size | 200 | <200 | empty | yes |
| TODO markers | 0 | 0 | n/a | yes |
| duplication | — | 0 | n/a | yes |
| clang-tidy | — | 0 | empty | unverified locally |
| **params** | 6 | **11** | **12 groups** | **no** |
| **DIP** | — | **unmeasured** | **none** | **no** |
| **`include/`** | — | **ungated** | **none** | **no** |

Five axes sealed, one unverified, three open. The three open ones are the proposal.

## Order

1. `include/` coverage (PR 1)
2. arch gate (PR 2) — the one you asked for
3. PARAM burn-down (PR 3)
4. `llvm@18` + version reporting (PR 5)
5. NLOC decision (PR 4) — your call, it is the one item I would argue you skip

## What I did not propose

Lowering caps to today's maxima to manufacture a green number. `NLOC_MAX = 39` and
`CCN_MAX = 13` would be trivially "spotless" and would fail on the next ordinary edit.
A cap one step below the worst current value is a cliff; a cap fitted to the present is
a measurement of nothing.
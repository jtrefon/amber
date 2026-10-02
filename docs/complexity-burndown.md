# Complexity burndown: to CCN 15 and a 40-line cap

Generated from `make complexity-report` and lizard. Regenerate rather than
trusting these numbers — this file has been wrong before, and the section
"Corrections" records how.

## Where we are

**2,349 functions**, **average NLOC 12.7**, **average CCN 3.5**. The codebase is
well-factored *on average*; the debt is concentrated in a few outliers, which is
what makes this tractable.

| cap (CCN <= 15) | over-limit | share of functions |
|---|---|---|
| **NLOC 50** | **0** | 0.0% |
| **NLOC 40 (enforced)** | **37** | 1.6% |
| NLOC 30 | 182 | 7.7% |
| **NLOC 25 (target)** | **281** | 12.0% |
| NLOC 20 | 442 | 18.8% |
| NLOC 15 | 709 | 30.2% |
| NLOC 10 (`AGENTS.md` aspiration) | 1087 | 46.3% |

**The project already passes a 50-line cap outright.** The enforced cap is 40,
and 37 functions are over it.

## The metric: NLOC, not total lines

The gate measures **NLOC** — non-comment, non-blank lines of code — and not
lizard's `length` (total physical lines).

This matters. Measured in total lines, the same codebase shows **81**
functions over 40 instead of 37, and the extra 44 are functions whose *code* is
already small but whose comments push the total over: `agent::decide_approval`
is 30 NLOC and 49 total lines. Gating on total lines would have created pressure
to **delete explanatory comments to pass the gate** — an incentive that makes a
codebase worse, and exactly the opposite of what a debt gate is for.

`length` is still printed by `--report`, for information.

## The gap worth naming

`AGENTS.md` documents the aspiration as *"A method/function should stay under 10
lines with minimal branching"* — which would flag **46% of every function**. That
is an aspiration, not a standard, and at 10 lines it is not reachable for C++:
RAII, error handling and templates all cost lines before any branching happens.

The enforced cap is 40, the target is 25, and the documented number is 10. This
document treats **25 as the target** and records 10 as aspirational only, so
nobody plans against a number the code cannot meet.

## Staged plan

The gate is a ratchet: it fails when a recorded function *grows* or a new one
appears. So the cap can be lowered one step at a time, with the baseline
regenerated at each step, and the gate is never "put up" ahead of the code. That
is the CX1 mistake in reverse — CX1 was a check switched on before the code
could pass it, which is why it sat deferred.

1. ✅ **CCN axis.** Done: **0** violations, empty baseline — a hard cliff.
2. ✅ **Length cap 50 -> 40.** Done, and the metric corrected to NLOC (see
   above). **37** functions baselined as the ratchet.
3. **Burn the 37 to zero** so the cap-40 baseline is empty and the axis becomes
   a hard cliff like CCN.
4. **NLOC 40 -> 30** (182).
5. **NLOC 30 -> 25** (281).

At each stage the baseline shrinks and the cap tightens, so the gate is always
green *and* always stricter than before. There is never a red build caused by
the cap alone.

## The fence: `complexity` is advisory, not gating

`complexity` is a CI job but is **not** in `ci-gate`, and branch protection
requires only `ci-gate`. So the gate can go red without blocking a merge — a
200-line, CCN-40 function can land today.

AGENTS.md documents this deliberately, but it means the CCN-at-zero achievement
is currently **unprotected**. The plan is to add it to `ci-gate` once the
baseline is empty (step 3): with no baseline file there is nothing to conflict
on, which is the reason the job was kept advisory in the first place.

## The ratchet is per-function

`tests/complexity_baseline.json` records each accepted over-limit function **by
name**, with its measured size:

```json
{ "lib/foo.cpp": { "agent::bar": { "nloc": 44, "ccn": 7 } } }
```

A file therefore cannot swap one over-limit function for another and pass — the
recorded function itself may not get bigger. (It used to record a per-*file*
count, which had exactly that hole.)

## Worklist

`make complexity-report` lists them, worst first. The 37 are all **long-only**
(CCN <= 15), between 41 and 49 NLOC — modest splits, not god functions.

## Corrections

- **2026-10-02** — the table above was previously computed from lizard's `length`
  (total lines), which overstated the debt by 2.2x and created a
  delete-the-comments incentive. Re-measured on NLOC; cap unchanged at 40.
- **2026-10-02** — a first attempt to count "functions over NLOC 40" ran lizard
  with `-L 10000`, which suppresses the warnings entirely (lizard warns on
  `length`, not NLOC), and reported a false **0**. The real number is 37.

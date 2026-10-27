# Context shaping: milestones

The compaction module is complete for its committed scope (epic #416, 17/18).
The tree shaker shipped only its two free seams: source projection for bulky bash
output, and deterministic in-turn digesting at rebuild. This document plans the
rest in **milestones**, each one pull request, each one independently shippable.

A milestone is the delivery unit. Features, red tests and refactors are commits
inside its PR. Do not open a PR per feature; the previous batch (one PR for four
features) is the shape we want.

## M1 — Agent-driven shaping

**Problem.** The model knows which evidence stopped mattering; the harness cannot
infer it. Today the agent can only wait for a compaction it does not control, and
the one rule that identifies redundancy (supersession) is measured at 0% because
no corpus lets the harness guess which read is stale.

**What ships.**

1. A model-facing `context` tool that submits a **plan**, never an edit:
   `{ "op": "drop" | "superseded", "result_ids": [...], "reason": "..." }`.
2. Validation at application time: an id is applied only if a live tool message in
   the context still carries it. Unknown ids are reported back, not guessed at.
3. Application at the next request boundary, once per user turn, and only when
   the freed content justifies the rebuild it causes. The model asked for a
   prefill; the harness refuses to spend one for nothing.
4. Every drop goes through the same digest path as compaction, so the payload
   stays recallable through `read_result`.
5. `superseded` as a selector: the harness computes the repeated-read set (the
   CC-18 rule), and the agent can ask for it without naming ids. This is where
   #467 closes: the rule is real, the *trigger* is the agent, not a timer.
6. Replay evidence: the harness reports what the tool would free on the recorded
   corpus before it is enabled by default.

**Out of scope.** No timer, no LLM adjudicator, no automatic application without
a request.

## M2 — Archive maintenance

**Problem.** The archive is capped and merged, never re-summarised. After several
compactions it holds many short entries and the working state thins out.

**What ships.**

1. On re-compression, the archive is re-summarised under a token budget instead
   of only being merged: newest entries verbatim, older ones folded.
2. Monotonic guard: a re-summarise that is not smaller is discarded.
3. KPI and scenario: three consecutive compactions on a growing session, with
   archive size, summary size and retention assertions.

**Out of scope.** No ranking model; the classifier remains the semantic channel.

## M3 — Prove it live

**Problem.** Every claim about classifier quality and prefill saving is
structural. The hermetic suite proves the harness, not the model.

**What ships.**

1. A repeatable live scenario suite with retention oracles: run N times against
   any OpenAI-compatible endpoint, score the answer and the tokens, report the
   spread.
2. A recorded baseline (as-is endpoint, no service changes) in the PR and the
   audit doc.
3. Prefill accounting where the endpoint exposes it; otherwise documented as
   structurally verified and not measured.

**Out of scope.** No inference-service changes. If the fixed model cannot
represent the population, that is a finding to report.

## After context

The following are already queued as their own milestones, in this order unless
redirected: memory/learn integrity (#444), observability (#443), git undo (#441),
profiles (#440), TUI queued prompts (#437).

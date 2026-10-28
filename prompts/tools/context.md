# context

The conversation carries tool results, and some of them stop being worth their
place: an early read that a later read replaced, a file you have already
summarised, output you have extracted what you needed from. `context` lets you
say which ones, and the harness replaces them with a short digest that keeps the
`result_id`, so `read_result` can bring any of them back byte for byte.

Two ways to name them:

- `op: "drop"` with `result_ids` — the ids you saw in a tool envelope's
  `meta={"result_id": ...}`, or in a digest you may already have.
- `op: "superseded"` with no ids — earlier reads of a path that was read again.
  The harness knows which those are; you only say that the duplicates can go.

Include a short `reason`. It travels with the plan and appears in the harness
report, so a later turn can tell why a result disappeared.

The plan lands at the start of your next request, not inside the current one:
the request you are building is already in flight. The harness applies it once
per user turn, and only when it frees enough to justify the rebuild that a
changed prefix costs. When a plan is too small, or names something no longer
present, the harness says so in a notice instead of guessing.

Trimming is about room, not tidiness. Drop what the task has finished with, keep
what a later step will quote, and leave the digest trail readable — a plan that
frees little is a plan that cost a prefill for nothing.

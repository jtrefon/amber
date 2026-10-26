# Context Compression Architecture

- **Status:** current (reconciled with the code in CC-12, 2026-10-26)
- **Spec:** `docs/spec/compression/compression-pipeline.md`
- **Audit:** `docs/deep-audit/context-compaction-review.md`
- **Design patterns:** Strategy (pipeline), Observer (progress/usage),
  Memento (a failed pass leaves the context untouched), Null Object (the
  observer's no-op defaults)

## The constraint

On local inference a full prefill of a 262K window costs minutes; extending a
cached prefix by a few hundred tokens costs a second. Compression exists to make
the *conversation* small again without paying a prefill for the history it is
about to remove.

Two consequences run through the code:

1. **The classify and extract calls replay the live request prefix** — the
   injected head/tail blocks and the tool schemas — plus one instruction. The
   conversation is never rewritten before it is sent; the free reduction pass
   runs afterwards, on the output.
2. **The context is rebuilt, never mutated.** `Agent::rebuild_context` is
   `clear()` + `push()`; the pipeline itself only reads a snapshot.

## Shape

```
gate (threshold · message floor · reducible history · user-turn cooldown)
  → compress(context, cfg, client, live_prefix)
      classify  ── one instruction appended to the live prefix
      extract   ── replays the classify pair, same prefix
      assemble  ── pure: tags → core / archive / prune, protected tail
      reduce    ── loop collapse, tool-output digests, reasoning dropped
      budget    ── target, then headroom (both protect the tail)
      invariant ── no reduction means the original history
  → rebuild (clear + push) · memory/skill ops · session brief
  → the normal request, on the compressed context
```

The pipeline is synchronous; the host may run it on a worker thread and uses the
observer for progress. `Context` keeps its single-owner contract: compression
reads a snapshot and returns a new list.

## What is protected, always

- The system prompt at index 0, whatever the classifier says.
- The **protected tail**: the newest real user turns that fit the tail budget,
  including the final prompt and everything after it. Tool results in that tail
  are never digested (that evidence is what the model is about to use).
- At least one real user message, and provider-valid tool call/result pairing.
- Harness notices are not user turns: they do not inflate the tail.

## What a pass produces

A single `system` message carrying the classifier's work-state summary and a
bounded archive of `context`-tagged runs. `prune` turns are gone. The message is
merged into the leading system block by every dialect on the wire; the classify
instruction says so when it updates a previous archive.

## Safety nets

| Net | Rule |
|---|---|
| purity | any classify/parse failure returns the original history |
| monotonicity | a pass that does not reduce returns the original (no summary for nothing) |
| budgets | target (`target_pct`) and headroom (derived below the trigger), measured after the archive is written |
| tail | token-budgeted; always includes the final prompt |
| pairing | orphan tool results removed, missing results stubbed |
| archive cap | 32 entries / ~800 tokens, oldest merged |
| cooldown | user turns; covers failed attempts; never blocks the first pass |

## Entry points

| Entry | Behaviour |
|---|---|
| `Agent::chat_once` | gate → `run_compression` → rebuild → request |
| `Agent::compress_now` (`/compress`) | bypasses the gate; reproduces the live prefix from the current context and registry |
| `CompressionStrategy::compress` | the pure pipeline: snapshot in, message list out |
| `CompressionObserver` | progress, per-call usage, parse/apply results; `CompressionReporter` is the host adapter |

## Testing

The invariants above are pinned by name in `tests/run_tests.cpp` and
`tests/agent_loop_test.cpp` (see the spec's scenario section), and
`bench/scenarios/compression/k-01-compression-stress.json` runs the real pipeline
hermetically with a ratio assertion. A live run uses the same scenario with
`--live` against an OpenAI-compatible endpoint.

## History

The 2026-10 review (`docs/deep-audit/context-compaction-review.md`) found the
original claims did not hold in production: the classify call sent no tools,
omitted the injected blocks and rewrote the history first, so every compaction
paid the prefill it was designed to avoid; the protected tail was a prompt count
that protected whole sessions; and the budget passes could emit an invalid
request or delete the active task. Those findings are fixed in the tree and
tracked as CC-01 … CC-14 in the epic (GitHub #416). This document describes the
shipped behaviour, not the 2026-07 design.

## Spec: Context Compression Pipeline

- **Status:** current (reconciled with the code in CC-12, 2026-10-26)
- **Source:** `lib/compressor.cpp`, `lib/compressor_apply.cpp`,
  `lib/compressor_parser.cpp`, `lib/compressor_request.cpp`,
  `lib/compressor_scanner.cpp`, `include/agent/compressor.h`, the gate and
  rebuild in `lib/agent.cpp`
- **Review:** `docs/deep-audit/context-compaction-review.md` (measurements and
  the remediation history)

### Purpose

Shrink a long conversation so the next turn is cheap, while keeping the active
task, the system prompt and the recent evidence intact. The pass runs at the top
of `chat_once`, before the request is built, when the gate says the context is
full enough to be worth it.

### Two claims that are true, and one that used to be false

1. **The pipeline is pure.** It reads a snapshot of the `Context` and returns a
   new message list. On any classify failure it returns the original list; the
   caller rebuilds with `clear()` + `push()` only on success.
2. **The two LLM calls share a prefix with each other.** The extract request
   replays the classify request (same prefix, same instruction, plus the
   classify reply), so the second call extends the first.
3. **The classify call replays the live request prefix** — the blocks the host
   injects after the system prompt, the blocks it appends after the
   conversation, and the tool schemas the turn sends. Without that the server
   cannot match its cache and pays a full re-prefill (measured 10–30 min on a
   262K window). The free reduction pass therefore runs on the **output**, never
   before the classifier sees the history.

   The `reasoning` field is not part of the wire and may be cleared anywhere.

### Pipeline

```
chat_once
  └─ gate (should_compress)
       └─ compress(context, cfg, client, prefix)
            1. classify  [system][head][history][tail][instruction] + tools
            2. extract   [same prefix][classify reply][extract instruction]
            3. assemble  apply_classification (pure, C++ only)
            4. free pass collapse_loops → prune_tool_io → drop reasoning
            5. budget    enforce_target_budget → enforce_headroom
            6. invariant1: no reduction → return the original
       └─ rebuild_context(clear + push) on success
       └─ apply memory/skill ops + session brief
  └─ inject_prompt_blocks (the same blocks the classify call replayed)
  └─ the normal request on the compressed context
```

### The gate

`DefaultCompressionGate` fires when all of:

| Condition | Detail |
|---|---|
| threshold | `prompt_tokens_used / context_size >= threshold` (default `0.70`). `prompt_tokens_used` is the server's last reported prompt size; the estimate is the fallback. An unknown window disables auto-compression — the gate never guesses. |
| message floor | `context.size() >= min_turns` (the name is historical: it counts **messages**). Bypassed when the **conversation** is at `>= 90%` of the window; the immutable system prompt does not count toward that share. |
| reducible history | more than one non-system message. A pass with only the system prompt and the opening ask would buy a summary of one sentence. |
| cooldown | `user_turn_counter - last_compress_turn < cooldown_turns` blocks. The unit is **real user turns**; one tool-using turn is many LLM calls and must not expire it. The first compression is never blocked: the cooldown protects a freshly compressed context, and a single-turn session may be the only chance it gets. |

The gate is checked before `inject_prompt_blocks`, so a rebuild can never lose
the blocks injected for the current request.

### The classifier

The instruction asks for a work-state summary plus per-range tags
(`core` / `context` / `prune`). It carries a **transcript index** — one line per
message (`index role preview`), newest messages individually and older ones
grouped, capped at ~2 000 tokens — because a model reading text cannot count
messages. The manifest is part of the instruction, so the cached prefix is
untouched.

When a previous compression is in the conversation the instruction says where
its archive really is: every dialect merges `system` messages into the leading
system block, so the archive is at the top of the wire request, not in the
transcript.

### Assembly

`apply_classification` is pure and applies the tags:

- the system prompt at index 0 is always restored, whatever the classifier said;
- the **protected tail** — the newest real user turns that fit the tail budget,
  always including the final prompt and everything after it — is forced to
  `core`. The tail budget is `target − max(512, target/10)` tokens; with an
  unknown window the prompt count (`keep_last_prompts`, default 10) is the cap.
  The classifier cannot tag *within* a turn (`ClassifiedSegment` is
  `turn_start`/`turn_end`), so in-turn relief comes from the free pass below;
- harness notices (`Message::harness`) do not count as user turns;
- `context`-tagged turns become one archive entry per contiguous run;
- `prune`-tagged turns are removed;
- the archive is capped at 32 entries / ~800 tokens; the oldest merge into one
  `earlier work, compressed` line;
- at least one real user message is guaranteed, and tool call/result pairing is
  repaired.

### Archive maintenance (re-summarise, not merge-only)

A merge that replaces the oldest entries with `earlier work, compressed` keeps
the archive bounded and throws the detail away, which is the wrong trade once a
session has been compacted several times: the working state thins out while the
archive stays the same size.

- **When.** A pass folds the carried archive when it has outgrown its cap (more
  than 32 entries, or more than ~800 tokens) *and* has entries older than the
  newest 8. An archive inside the cap costs no call — there is nothing older to
  fold.
- **What.** One standalone request carries the entries to fold and asks for a
  single shorter entry: facts a later turn depends on, no narration, no JSON.
  The newest **8** entries stay verbatim, so recent work is unchanged, and the
  merge-only cap stays as the fallback when the call fails.
- **Order.** The fold runs after classify and extract, on a working copy: those
  two still replay the live prefix byte for byte, and a failed pass still
  returns the original history untouched. The fold request is not prefixed with
  the conversation — the archive is its payload, so replaying the live prefix
  would pay the context-sized prefill the pipeline exists to avoid.
- **Monotonic.** A fold that is empty, or that does not make the archive
  smaller, is discarded and the entries it would have replaced are kept: a
  "summary" that costs as much as its input has thrown the detail away for
  nothing. `cap_archive()` still bounds the result.
- **Bounded.** Three consecutive compactions on a growing session end with an
  archive at or under the cap, the work-state summary intact, and a fact the
  newest window carried still present (`three_compactions_keep_a_bounded_archive_and_the_work_state`).

The compressed-context message is a `system` message carrying `{archive,
summary}`. Nothing else: `type`, `version` and `facts` had no reader.

### Budgets

- **Target** (`target_pct`, default 10% of the window): archive eligible core
  messages until the output fits. The pass reserves the archive entry it is
  about to write, so it cannot overshoot by its own bookkeeping.
- **Headroom**: derived from the trigger, `min(75%, max(10%, threshold − 10%))`
  of the window. It must leave the history *below* the trigger that fired it, or
  a compaction can qualify again the moment the cooldown lapses.
- Both passes protect the same tail and end with the same finaliser (pairing
  repair + minimum-context guarantee + re-measure).

### In-turn pruning

A session whose only content is one oversized turn has nothing older for the
classifier to archive: the protected tail is the whole conversation. The free
pass therefore also trims *inside* the tail — newest first, keeping the tool
results that fit the tail budget and digesting the older ones. The properties:

- **Deterministic.** No classify, no extract, no summary: a classify request
  replays the live prefix, so using it to decide what to prune would pay three
  context-sized prefills to do what pruning does for one. The pipeline takes this
  path only when the turn actually has something to digest; otherwise it runs
  the classifier and reports its honest "no reduction".
- **Newest survives.** The most recent tool result is never replaced, even when
  it alone exceeds the budget; the budget decides how much older evidence joins
  it. Assistant messages and the final prompt are never touched.
- **Recallable.** Every digest carries a `result_id` (stored on demand by the
  compactor, not only by the 32 KiB page-out), so `read_result` restores the
  exact envelope the model saw.
- **Idempotent.** A message that is already a digest is skipped, so a second
  pass cannot digest the index away.

### Shaping on request (the model's own trim)

Compaction decides what to keep from a summary of the session. Only the model
knows which evidence has stopped mattering — an early read a later read
replaced, output it already extracted a fact from. The `context` tool is that
judgement as a request: the model names results, the harness validates and
applies them.

- **A plan, never an edit.** `{"op": "drop", "result_ids": [...]}` or
  `{"op": "superseded"}`, plus a short reason. The tool only pushes a
  `ShapePlan` onto the host's `ShapeQueue`; it never touches the context. The
  agent drains the queue at the top of its next request, so the request it was
  building is not modified underneath it.
- **Validated against the live history.** A `drop` applies only to ids a live
  tool message still carries; ids that are gone are reported back, never
  guessed at. `superseded` is the CC-18 rule (earlier reads of a path read
  again), chosen by the agent instead of a timer.
- **At most once per user turn, above a floor.** A rebuild changes the prefix,
  which is a full prefill. The harness applies one plan per user turn and only
  when it frees at least **2 KiB**; below that, or on a second plan in the same
  turn, a `[harness]` notice says what happened and why.
- **Recallable.** Every replacement goes through the same digest path as
  compaction, so the digest carries the `result_id` and `read_result` serves the
  original byte for byte. A wrong drop costs a tool call, not the evidence.
- **No timer.** Nothing is shaken on a schedule: shaping happens because the
  model asked, at a boundary it did not have to guess at (D7, D9).

**Naming at the source.** A model cannot name what has no id, and only results
the envelope paged out (over 32 KiB, or `bash` over 8 KiB) carried one. Every
result the compactor can digest — anything over **200 bytes**, the same
threshold the digest uses — is now stored and named at dispatch, so a plan can
point at it before it is ever destroyed. The cost is one small file per
non-trivial result, under the same retention as any other artefact.

**What it can free (replay, measured).** `amber-bench shape-replay --window
32768` over the 30 largest recorded sessions (1,540,531 tool tokens):

| rule | tokens saved | replaced | referenced later |
|---|---|---|---|
| in-turn (shipped) | 5.8% | 74 | 30 |
| bash projection (shipped) | 2.8% | 11 | 2 |
| superseded reads | 67.7% | 462 | 462 |
| drop everything | 90.5% | 832 | 533 |

The last two are the ceiling, not a prediction: `drop-all` is what a perfect
planner could reach if nothing it dropped was needed, and 533 of its 832
payloads are named again later. `superseded` marks every dropped read as
referenced because the later read of the same path repeats the same lines — the
count cannot tell a stale copy from a different range, which is why the trigger
is the agent and not the metric. The free seams are the floor; the gap between
them is what the model's judgement is for.

### Source projection (before the context)

The cheapest shaping happens before a result is ever pushed: `page_out_oversized`
stores bulky `bash` output and caps its envelope at **8 KiB**, keeping the head
and the tail (the command echo and the failure) around an omission marker with a
`read_result` handle. `read` is never projected — the model asked for that
range — and `search` is already capped by its own `max`. Measured on 24 recorded
sessions: 3% of tool tokens, 7 results, one dropped span later named (and
therefore served by the handle).

### Re-arming a compaction

`compression_cooldown_turns` counts **real user turns**
(`Config::user_turn_counter`), so one tool-using turn — many LLM calls — cannot
expire it. That is the right unit for rate-limiting a user, and the wrong wall
for a long turn: a single turn can refill the window several times and would get
no relief until the next message. The gate therefore re-arms as soon as the
prompt has grown by 10% of the window (minimum 1024 tokens) since the last
attempt. That is a real consumption step and cannot spin: an attempt that
reduced nothing left the size unchanged.

### Invariants

| ID | Invariant |
|---|---|
| I1 | A compaction never returns a history larger than its input; if it cannot reduce, the original is returned untouched. |
| I2 | The rebuilt history is provider-valid: every `tool` result follows an assistant `tool_calls` that names it, and every call has a result or a stub. |
| I3 | At least one real user message survives. |
| I4 | The final user prompt, every assistant message and the newest tool results of the active turn survive. Older results of that turn may be replaced by a digest that carries a `result_id` (recallable through `read_result`); nothing newer than the digest cut is touched. |
| I5 | The output fits the target budget, measured **after** the archive is written. |
| I6 | `headroom_budget < trigger` for every configuration. |
| I7 | The classify request replays the live request prefix (blocks + tools); the extract request replays the classify request. |
| I8 | The archive is bounded (entries and tokens) and the summary is always carried. |

### Failure modes

| Failure | Behaviour |
|---|---|
| classify LLM error | return the original; `CompressionResponse::error` set; the cooldown covers the attempt; a `[harness]` note tells the model |
| classify reply unparseable | same as above |
| extract error or unparseable | classification stands; only memory/skill ops and the brief are missing (CP-08) |
| no reduction | return the original silently (no error): the cooldown applies, nothing is written |
| archive missing on the output | budget passes bail rather than drop what they cannot record |
| unknown window | auto-compression disabled; `/compress` still works |

### Configuration

| Key | Default | Meaning |
|---|---|---|
| `compression_threshold` | 0.70 | fraction of the window that triggers a pass |
| `compression_min_turns` | 10 | message floor (bypassed at 90% conversation saturation) |
| `compression_cooldown_turns` | 20 | user turns between attempts (attempts, not just successes); re-arms early on 10% window growth |
| `compression_target_pct` | 10 | post-compaction target, % of the window |
| `compression_keep_last_prompts` | 10 | maximum user turns kept verbatim (the token budget decides the rest) |

### Known gaps (measured, not assumed)

1. **The archive fold is one level deep.** Old entries are folded into one
   summary, and a later pass folds that summary again with anything newer; the
   detail that falls out of the newest window is not recoverable from the
   archive. The session log keeps the transcript, but the archive is a summary,
   not an index.
2. **The shaping queue is one per process.** A plan names ids; an id another
   window's or a sub-agent's context does not carry is refused, so a plan can
   only ever apply to the history that asked for it. A queue per window would
   save the refusal notice, not change what is applied; a plan raised inside a
   sub-agent is refused for the same reason, and the refusal is reported.
3. **Provider prompt caching is modelled only where it is explicit.** Anthropic's
   Messages API takes `cache_control` breakpoints and amber sets them (stable
   system block + moving breakpoint on the newest turn, TTL from
   `/set provider cache`). OpenAI-compatible endpoints and Gemini cache
   automatically; amber does not create Gemini `cachedContent` resources. See
   `docs/spec/llm-client/prompt-caching.md`.
4. **The classifier's index grounding is a heuristic.** The manifest gives it
   numbers; a mis-tagged range is still possible. The tail and default-core
   rules make the failure safe, not accurate.
5. **In-turn compaction keeps the newest evidence, not all of it.** Older
   results of the active turn become digests once the tail budget is spent; they
   are recallable through `read_result`, not present in the prompt. A session
   with one enormous result that must be read whole gets one rebuild, not a
   smaller reading of it.

### Scenarios

The scenario set (CP-01 … CP-19) and the acceptance criteria live in
`tests/run_tests.cpp` and `tests/agent_loop_test.cpp`; the benchmark scenario
`bench/scenarios/compression/k-01-compression-stress.json` runs the real pipeline
hermetically and asserts a compaction ratio and the final answer. The named
tests are the authority; this file describes the behaviour they pin.

### Cross-references

- `loop-collapse.md`, `turn-classification.md` — the two reductions in detail
- `docs/spec/context/context-ownership-and-parallel-compression.md` — the
  sealed-stack and single-owner contract
- `docs/spec/benchmark/kpi-framework.md` — `min_compactions` and
  `max_compaction_ratio`
- `docs/deep-audit/context-compaction-review.md` — the audit and measurements

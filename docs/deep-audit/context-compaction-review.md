# Context Compaction — Architecture Review

- **Status:** Review complete. Remediation tracked in the `context compaction`
  epic ([#416](https://github.com/jtrefon/amber/issues/416)); the
  [remediation map](#8-remediation-map) lists the child issues and GitHub is the
  live work register.
- **Reviewed revision:** `c2284db1` (`main`, 2026-10-26)
- **Scope:** `lib/compressor.cpp`, `lib/compressor_apply.cpp`,
  `lib/compressor_parser.cpp`, `lib/compressor_request.cpp`,
  `lib/compressor_scanner.cpp`, `include/agent/compressor.h`, the gate and
  rebuild in `lib/agent.cpp`, the wire merge in `lib/dialect_*.cpp`, the
  compression tests, the compression bench scenario, and the three design docs.
- **Method:** line-by-line trace of every path, then hermetic probes (recorded in
  [Appendix A](#appendix-a--probe-results)) against `libagent_core.a` with
  synthetic histories. No live inference was used or reconfigured.
- **Not measured:** real prefill wall-clock on the local 27B/262K endpoint. The
  prefix analysis below is structural (token-stream identity), not timed.

---

## Table of contents

1. [Verdict](#1-verdict)
2. [What holds up](#2-what-holds-up)
3. [Findings](#3-findings)
   - [C1 — the KV-reuse premise does not hold](#c1--the-kv-reuse-premise-does-not-hold)
   - [C2 — pruning erases the active turn's evidence](#c2--pruning-erases-the-active-turns-evidence)
   - [C3 — budget passes break pairing and can delete the task](#c3--budget-passes-break-pairing-and-can-delete-the-task)
   - [H1 — the protected tail is a prompt count, not a budget](#h1--the-protected-tail-is-a-prompt-count-not-a-budget)
   - [H2 — the archive costs more than it tells the model](#h2--the-archive-costs-more-than-it-tells-the-model)
   - [H3 — the fence stripper corrupts JSON escapes](#h3--the-fence-stripper-corrupts-json-escapes)
   - [H4 — headroom is above the trigger](#h4--headroom-is-above-the-trigger)
   - [H5 — deprecation is unreachable end to end](#h5--deprecation-is-unreachable-end-to-end)
   - [M1 — write-only payload and a mislabelled fact](#m1--write-only-payload-and-a-mislabelled-fact)
   - [M2 — the gate's guards count the wrong thing](#m2--the-gates-guards-count-the-wrong-thing)
   - [M3 — the monotonic invariant is unenforced](#m3--the-monotonic-invariant-is-unenforced)
   - [M4 — the docs contradict the code and each other](#m4--the-docs-contradict-the-code-and-each-other)
   - [M5 — module hygiene and API honesty](#m5--module-hygiene-and-api-honesty)
   - [M6 — retention and ratio are unmeasured](#m6--retention-and-ratio-are-unmeasured)
4. [Performance](#4-performance)
5. [Retention](#5-retention)
6. [The classification contract](#6-the-classification-contract)
7. [Target state](#7-target-state)
8. [Remediation map](#8-remediation-map)
9. [Parallel execution plan](#9-parallel-execution-plan)
10. [Test plan](#10-test-plan)
11. [Appendix A — probe results](#appendix-a--probe-results)
12. [Appendix B — documents that must change](#appendix-b--documents-that-must-change)

---

## 1. Verdict

The **structure** is good: clean ports, a pure pipeline, an atomic failure path,
and a `Context` rebuild that honours the sealed-stack contract. The **premise is
not true in production**, and two post-processing passes can emit a history that
is either wire-invalid or has no user task in it.

| # | Finding | Class | Priority |
|---|---|---|---|
| C1 | Compression requests are not the live prefix (missing `tools`, missing head blocks, mutated messages) → every compression pays the full prefill it exists to avoid | premise | P1 |
| C2 | `prune_tool_io` replaces every tool output >200 chars **including the current turn's last result** | data loss | P0 |
| C3 | `enforce_target_budget`/`enforce_headroom` run after `sanitize_tool_pairs` and can orphan a `tool` result or delete the only user message | integrity | P0 |
| H1 | `keep_last_prompts` is an absolute count, so the 10% target is unreachable; reduction can be negative | ability | P1 |
| H2 | One content-free archive entry per dropped message; the archive alone can equal the target budget | ability | P1 |
| H3 | Fence stripping deletes `\n` escapes inside valid JSON (silent corruption, or an aborted pass) | parser | P1 |
| H4 | Headroom budget (75%) is above the gate trigger (70%) | consistency | P2 |
| H5 | `deprecate` is filtered, content-keyed and undocumented — three independent dead ends | dead path | P2 |
| M1–M6 | Write-only payload, gate guard semantics, unenforced invariant, doc rot, linkage/API hygiene, no measurement | quality | P2–P3 |

**In one sentence:** amber's compaction works today mostly because a 200-character
tool-output filter removes 98–99% of the tokens, while the two LLM calls the
design is built around cannot reuse the KV cache and its bookkeeping erodes the
result.

---

## 2. What holds up

Worth stating explicitly, because these are the parts a remediation must not
break:

- **The `Context` contract is honoured.** Rebuild is `clear()` + `push()`
  (`lib/agent.cpp:637-642`); the pipeline never mutates the live stack.
- **Atomic failure is real.** A classify/parse failure returns `original`
  (`lib/compressor.cpp:389-395`), and `fail_compression_after` leaves the context
  untouched while still reporting the attempt (`lib/agent.cpp:597-614`).
- **The gate asks the right question.** `should_compress` reads server-truth
  `prompt_tokens_used` when it has it, clamps with the learned window, refuses to
  guess an unknown window, and its cooldown covers *failed* attempts
  (`lib/compressor.cpp:57-94`, `626-632`).
- **Gate placement is deliberate.** It runs before `inject_prompt_blocks`
  (`lib/agent.cpp:511-522`) so a rebuild cannot silently drop the injected
  blocks — a real bug that was fixed once and is guarded by a test.
- **Archive carry-forward** (`carried_archive` + the `update_previous` suffix) is
  a thoughtful mechanism: re-compression extends rather than restarts.
- **Test breadth.** ~50 tests cover the parser, classifier application, gate,
  headroom, target budget, briefs and the pipeline (with `FakeLLMClient`). The
  module is not untested; it is tested for *isolated correct behaviour* and not
  for the *invariants the design claims*.
- **Size gates pass.** `make complexity`: no compression function over the caps.
  `tools/nesting_gate.py`: 0/3514 over depth 4.

---

## 3. Findings

### C1 — the KV-reuse premise does not hold

The module's whole justification is "zero full prefills during compression"
(`docs/architecture/context-compression.md:42-54`,
`docs/spec/compression/compression-pipeline.md:5-13`). For the request to extend
the cache, its token stream must equal the *live turn's* stream up to the
instruction. Three independent facts break that, and any one is sufficient:

**1. The compression calls send no tools.** `client.chat(request, {}, &stats)` at
`lib/compressor.cpp:280` and `:340`. A normal turn sends the registry
(`lib/agent.cpp:524-530`). Chat-template servers (llama.cpp) render the tool
schemas at the *start* of the prompt; hosted APIs keep them in a hidden system
block. Either way the cacheable prefix differs from token 0.

**2. Live turns carry head blocks the compression request does not.**
`inject_prompt_blocks` inserts memory and skill-discovery blocks *immediately
after the system prompt* (`lib/agent.cpp:975-993`, `1041-1074`), and
`insert_prompt_blocks` is only ever applied to `prompt_copy`, never to
`context_`. A session with any matched memory or skill (the normal case once
compaction has run once) therefore has a live prefix
`[system][memory][skills][conv…]` while the classify request is
`[system][conv…][instruction]`.

**3. The pre-pass rewrites the history before sending it.** `collapse_and_prune`
runs at `lib/compressor.cpp:379`, *before* `run_classify`:
`collapse_loops` deletes messages (`lib/compressor_scanner.cpp:57-94`) and
`prune_tool_io` replaces every tool payload >200 chars with a digest
(`lib/compressor_apply.cpp:355-384`). The classify request is a *different*
conversation, by construction.

Only the `m.reasoning.clear()` step is KV-safe: the dialect never emits
`reasoning` (`lib/dialect_openai.cpp:619-641`), so it does not change the wire.

**Why the tests don't catch it.** `compression_extract_request_shares_classify_prefix`
pins *classify → extract* identity, which is true and easy. The property that
matters — *live request → classify* identity — has no test. The suite measures
the thing next to the thing.

**Impact.** At a 70% trigger on a 262K window, each automatic compaction pays a
full prefill of ~183K tokens (the 10–30 min the design exists to avoid) *plus*
the unavoidable prefill of the compressed result. The two-call design's entire
token budget (~800 new KV tokens) is consumed by the first call's re-prefill.

**Direction.** Either restore identity — pass `tools`, insert the same head
blocks, classify the *unmodified* snapshot and move the pre-pass to the apply
side — or drop the claim and collapse to one call. See CC-04 and decision D1.

---

### C2 — pruning erases the active turn's evidence

`prune_tool_io(copy, &cfg)` sets `limit = history.size()` and prunes **the whole
history**, including the current turn's last tool result
(`lib/compressor_apply.cpp:362-382`, threshold 200 chars).

The gate is checked at the top of *every* `chat_once`, i.e. immediately after
tool results were pushed. Sequence:

```
assistant tool_calls → tool results pushed → chat_once → gate fires
→ prune_tool_io replaces the result the model just asked for
→ model must reason about "[tool output omitted: … first="…" last="…"]"
```

It can re-read, but not idempotently: the file may have changed, the command may
not be safe to repeat, and it costs a round trip. The 200-char threshold means a
*single* small file read is destroyed.

The session log keeps the original, but the model cannot read the log — that
mitigation only helps a human.

**Direction.** Prune only outside the protected tail (the helper already exists:
`protected_tail_start`). Aggressive pruning of *older* tool output is where the
real win is; the current turn's evidence is where task success lives.

---

### C3 — budget passes break pairing and can delete the task

`sanitize_tool_pairs` runs inside `apply_classification`, **before** both budget
passes (`lib/compressor.cpp:300-307`):

```
apply_classification   → sanitize_tool_pairs   (pairing valid)
enforce_target_budget  → drops messages        (pairing can break)
enforce_headroom       → drops messages        (pairing can break)
```

Measured with a budget sweep over a 10-round tool session:

| Pass | Budgets producing an orphan `tool` result |
|---|---|
| `enforce_headroom` | **5 / 50** |
| `enforce_target_budget` | **3 / 39** |

An orphan is a `tool` message whose preceding assistant `tool_calls` was dropped.
llama.cpp chat templates and OpenAI both reject that sequence, so the next turn
400s.

Same root cause, second symptom: `enforce_headroom` has **no protected tail** and
only skips `system` messages (`lib/compressor_apply.cpp:487-495`). When the tail
alone exceeds the budget it deletes the active user prompt:

```
in:  [system "S"][user a][archive][assistant thoughts]["THE ACTIVE TASK" 4 KB]
enforce_headroom(window=900 → budget 675)
out: [system "S"][system archive]        ← no user message at all
```

`ensure_user_message` (CP-16) is enforced inside `apply_classification` and then
undone by the passes that follow.

Third symptom: both passes compute `used` *before* writing their own archive
entries, then grow the archive afterwards. Measured: `enforce_headroom` finished
at **3350 tokens against a 3072 budget** — a post-condition it just failed, with
an overshoot proportional to the number of drops.

**Direction.** One `finalize()` step after all passes: `sanitize_tool_pairs` +
`ensure_user_message` + re-measure + assert. Give `enforce_headroom` the same
protected tail as the target budget. Assert the post-condition in debug builds.

---

### H1 — the protected tail is a prompt count, not a budget

`protect_active_task` marks the last `keep_last_prompts` (default **10**) user
prompts **and everything after them** as `core`
(`lib/compressor_apply.cpp:49-62`, `290`). `protected_tail_start` applies the
same rule to both budget passes (`:191-200`).

Measured:

| Session | Messages | Tokens | Result |
|---|---|---|---|
| 1 user prompt, 20 tool rounds | 42 → 43 | 9 675 → **9 717** | classification is a no-op; output is *larger* |
| 12 prompts, 20 rounds | 493 → 412 | 116 023 → 96 729 (−17%) | target was 26 214 (−77%) |

The design guarantees that at most `total_prompts − 10` prompts can ever be
archived, no matter how large the window is. In a long single-turn agent session
(the common case: one request, 50 tool calls) compaction cannot archive anything
at all — it still pays two LLM calls and appends a summary + archive message.

**Direction.** Express the tail as a **token budget** (`tail_tokens`, capped by
the window), and keep prompts verbatim only while they fit. Then assert
monotonicity (see M3).

---

### H2 — the archive costs more than it tells the model

Every dropped message becomes its own archive entry, with the same content-free
note (`lib/compressor_apply.cpp:204-216`):

```json
{"turns": "12", "summary": "(compressed to meet target budget)"},
{"turns": "13", "summary": "(over-budget archived)"},
```

Measured (`context_size=8192`, `target_pct=10` → target 819 tokens):

```
dropped messages: 38
archive entries:  38
archive message:  762 tokens   ← 93% of the target budget
```

The archive is a `system` message, so it counts against `used` and can never be
trimmed by the passes that grow it; `carried_archive` carries every entry into
the next compaction forever. The compressed context converges to the archive.

Also write-only: `type`, `version`, `facts` and `facts.last_goal` have **no
reader** anywhere in `lib/`, `tui/`, `src/` or `bench/` — see M1.

**Direction.** One entry per contiguous dropped run with a count and a real
summary; a hard cap (entries *and* tokens) with re-summarisation on
re-compression; drop the write-only payload.

---

### H3 — the fence stripper corrupts JSON escapes

`strip_fences` deletes the two characters `\` + `n` **anywhere** in the text
(`lib/compressor_parser.cpp:29-41`). It only runs when the raw reply does not
parse — i.e. exactly when the model wrapped its JSON in a fence, the case it
exists for.

```
```json {"summary":"line one\nline two", …} ```
→ summary = "line oneline two"              (silent corruption, still parses)

"see C:\\new\\file.txt"
→ \n removed at the wrong place → \e (parse error) or \b (valid escape, corrupt)
```

A fenced reply with an escaped newline in a "dense paragraph" summary is likely.
A parse error aborts the whole compaction; a silent one loses formatting in the
summary the agent must continue from.

**Direction.** Remove fence *lines* only (leading/trailing ```` ``` ```` markers);
never touch escapes inside the JSON body. Keep slicing/prose recovery as-is.

---

### H4 — headroom is above the trigger

`enforce_headroom` targets `0.75 × window` (`lib/compressor_apply.cpp:492`); the
gate fires at `0.70` (`include/agent/compressor.h:28`). A pass is therefore
allowed to finish in a state that satisfies the trigger again as soon as the
cooldown lapses, and the docs assert a third story ("25% free"). The relation
`headroom_budget < trigger_threshold` is a requirement, not a coincidence.

---

### H5 — deprecation is unreachable end to end

Three independent blockers:

1. `parse_knowledge_ops` drops any op with empty `content`
   (`lib/compressor_parser.cpp:121-129`), so `{"name":"x","action":"deprecate"}`
   is discarded.
2. `MemoryStore::deprecate` keys on **content**
   (`include/agent/experience.h:66-73`): the model must reproduce a stored
   sentence byte-for-byte.
3. The extract prompt never documents `deprecate`
   (`lib/compressor_request.cpp:70-95`), so the model cannot emit the documented
   shape.

`apply_memory_ops` also claims to "flag as conflict so the LLM picks a different
name" (`lib/compressor_apply.cpp:421-427`) while the conflict only reaches the
UI — the LLM never sees it.

**Direction.** Make deprecation name-keyed, document it in the extract prompt and
keep it, or delete the branch and the store method. Do not leave a third path.

---

### M1 — write-only payload and a mislabelled fact

`compressed_context_message` scans **forward** and breaks on the first short user
message, storing it as `facts.last_goal`
(`lib/compressor_apply.cpp:146-152`). It is the *first* goal, not the last, and
nothing reads `facts`/`type`/`version` at all. It is permanent per-request
payload for a fact that is both wrong and unused.

### M2 — the gate's guards count the wrong thing

`sufficient_turns` compares `context.size() >= min_turns` — **messages**, not
turns (`lib/compressor.cpp:76-78`). A single giant turn (1 prompt + 8 large tool
results) can exceed the window with fewer than 10 messages, so the gate never
fires and the next request overflows. `cooldown_turns` is compared against
`turn_counter_`, which increments per **LLM call** (`lib/agent.cpp:539`), so a
"20-turn" cooldown expires inside a single user turn.

**Direction.** Rename to what they count, and bypass the turn floor near
saturation (`utilisation >= 0.9`) so a short-but-huge session can still compact.

### M3 — the monotonic invariant is unenforced

Spec invariant 1 says the message count never grows
(`docs/spec/compression/compression-pipeline.md:75`); CP-11 rationalises the
opposite (`:308-313`). Measured growth: 42 → 43 messages, 9 675 → 9 717 tokens.
Either assert it (`after <= before + one archive message`) or change the spec.

### M4 — the docs contradict the code and each other

| Doc claim | Reality |
|---|---|
| "pipeline operates on the live `Context` via push/pop" (`architecture:279-281`) | pure copy-based pipeline |
| "gate is checked after each turn, not inside `chat_once`" (`architecture:285-287`) | it is inside `chat_once` |
| `enforce_minimum_context`, `split_classify_response` | do not exist |
| "32 000-token `kFallbackContextBudget`" (`spec:186-194`) | does not exist; unknown window disables the gate, as AL-11 says |
| "Known gap: headroom not implemented" (`spec:412`) | implemented |
| "Known gap: no mock LLM in compression tests" (`spec:406`) | `FakeLLMClient` pipeline tests exist |
| "walks *backward*, reclassifies as context" (`architecture:191`) | drops oldest, records indices |

The `docs/spec/INDEX.md` also advertises `compression/loop-collapse.md` and
`turn-classification.md`, which exist but describe a design the code does not
implement.

### M5 — module hygiene and API honesty

- **Seven functions have external linkage but no declaration**:
  `agent::parse_segment`, `parse_brief`, `string_list`, `parse_turn_range`,
  `parse_knowledge_op`, `parse_knowledge_ops`, `parse_response_body`
  (`nm -C lib/compressor_parser.o`). Generic names in `agent::` are a
  link-collision waiting to happen.
- `make_compressor(const CompressionConfig&)` ignores its argument
  (`lib/compressor.cpp:405-408`) while the gate stores a copy and the pipeline
  re-reads per call: three config lives.
- `CompressionStrategy::compress(Context&, …)` never mutates — should be
  `const Context&` (`include/agent/compressor.h:172-175`).
- `append_message(std::vector<Message> base, Message m)` copies the whole history
  for the first call of each step; `compress()` holds three full copies
  (`original`, `copy`, `classify_prefix`). Immaterial next to a prefill, free to
  fix.
- `CompressionReporter` (a UI presenter that formats status strings) shares a TU
  with the gate, the pipeline and the config loader — four responsibilities.
- `run_classify` takes 6 parameters + 2 out-params (the only compression function
  lizard flags, 47 lines).

### M6 — retention and ratio are unmeasured

The only compression bench scenario
(`bench/scenarios/compression/k-01-compression-stress.json`) feeds the
**legacy bare-array** classifier reply with **no summary**, and is
`hermetic_only`. It cannot detect a summary regression, a ratio regression or a
retention failure. Everything else is unit-level.

`AGENTS.md` requires prompt changes to be proven with a before/after benchmark;
the compression prompts are inline C++ literals that no benchmark exercises.
**There is no evidence in the repo for the advertised 2K–10K-token result or for
retention quality.**

---

## 4. Performance

Measured on synthetic histories (probe P5, Appendix A):

| Session | Window | Input | After pre-pass | After LLM + budgets | Pre-pass share |
|---|---|---|---|---|---|
| 1 prompt / 90 rounds × 4 KB | 262 144 | 93 015 | 1 284 | 1 316 | **98.6%** |
| 3 prompts / 30 rounds × 4 KB | 262 144 | 93 183 | 1 452 | 1 484 | 98.4% |
| 12 prompts / 20 rounds × 4 KB | 262 144 | 247 989 | 2 686 | 2 718 | 98.9% |
| 30 prompts / small outputs | 262 144 | 26 121 | 3 650 | 3 682 | 13.4% |

Interpretation:

- **The 200-character tool-output filter is the whole compression.** It removes
  98–99% of a tool-heavy session before any LLM call. The classifier contributes
  the summary text and (in principle) the archive, but not the token outcome.
- **The LLM's cost is therefore poorly matched to its value**: two calls per
  compaction, one of which pays a full re-prefill (C1), to summarise a history
  the pre-pass has already reduced to ~1% of its size.
- **Copies** are ~5 deep copies per compaction (a few MB at 262K tokens,
  milliseconds). Negligible today, pointless regardless.
- **Cancellation**: a classify call that triggers a cold prefill cannot be
  interrupted meaningfully (`run_compression` clears the cancel flag on entry,
  `lib/agent.cpp:680`). With C1 fixed this matters less; today it is a
  30-minute unkillable worker.

---

## 5. Retention

What actually survives a compaction today:

| Information | Channel | Verdict |
|---|---|---|
| Active task / recent prompts | protected tail, verbatim | ✅ safe — unless C3's headroom pass reaches it |
| Current turn's tool results | `prune_tool_io` digest | ❌ **lost above 200 chars, before the model sees them** |
| Older tool results | digest (first/last line, status, bytes) | ⚠️ lossy but useful |
| Decisions, constraints, identifiers | classifier's free-text summary | ⚠️ one paragraph, paraphrased, untested, unbounded |
| `context`-tagged turns | `{"turns": N, "summary": …}` ≤200 chars | ⚠️ only if tagged; summary text is model-chosen |
| Budget-dropped turns | `{"turns": N, "summary": "(compressed…)"}` | ❌ no content at all |
| Durable facts | memory/skill store (extract step) | ⚠️ works, but upserts bypass evidence (`promoted = true`) and deprecate is dead |

The summary prompt (`lib/compressor_request.cpp:8-42`) is the strongest part of
the module: a work-state framework (what is being worked on, what is done,
where the code lives, what remains) rather than tag mechanics. It is just never
measured, and it is derived from a history whose tool evidence the pre-pass has
already replaced with digests.

Missing by design:

- **Exact identifiers.** No channel pins function names, error strings, command
  lines, signatures or line numbers. The summary may paraphrase them away.
- **Pinned facts.** There is no "do not lose these" channel; the archive cannot
  carry verbatim content.
- **Summary bounds.** `kMaxSummaryChars` caps *segment* summaries only; the
  work-state summary is unbounded.

---

## 6. The classification contract

Two structural problems that must be settled before any accuracy work:

**1. The index space is ungrounded.** The prompt asks for
`{"turns":"0-5","tag":…}` (`lib/compressor_request.cpp:8-42`), but the model sees
text, not indices. It cannot count messages reliably and nothing gives it a
legend. The mitigations (default `core`, clamping, the protected tail) make the
failure mode *safe*, not the classification *accurate* — which is why in practice
the archive is nearly empty and the LLM's main product is the summary.

**2. The archive moves on the wire.** `append_messages` merges every `system`
message into one block emitted at position 0 (`lib/dialect_openai.cpp:594-661`;
the same in `dialect_anthropic.cpp:610-622` and `dialect_gemini.cpp:211-222`).
The compressed-context message is physically *last* in the harness but *first* on
the wire, so:

- the update suffix "classify only the turns that came AFTER that message" is
  incoherent to the model;
- every index after the archive shifts by one;
- the model's mental model of the list differs from `tag_turns`'s, which is what
  the ranges are applied to.

**Compatible resolution (decision D2).** Keep the live prefix intact and append
a **bounded manifest** to the classify *instruction* (not to the conversation):
one line per message or message group — `index → role → 60-char preview`, oldest
groups collapsed, capped at a few thousand tokens. That grounds the ranges
without touching the cached prefix, and it stays compatible with CC-04. Also
state the archive's actual position in the instruction whenever `update_previous`
is set.

---

## 7. Target state

### Invariants (each becomes a test)

| ID | Invariant |
|---|---|
| I1 | A compaction never increases the token count by more than the one compressed-context message it appends. |
| I2 | The rebuilt history is API-valid: every `tool` result follows an assistant `tool_calls` that names it; every assistant `tool_calls` has a result (or a stub). |
| I3 | After any compaction, at least one real user message (not a harness notice) survives verbatim. |
| I4 | No tool result at or after the protected tail start is replaced or dropped. |
| I5 | The output fits the target budget, measured **after** the archive is written. |
| I6 | `headroom_budget < trigger_threshold` always (configuration cannot invert it). |
| I7 | The classify request is a token-prefix of the last live request, plus the new instruction — or the module documents that it is not. |
| I8 | The archive is capped (entries and tokens) and carries a summary, not a per-drop index list. |

### Decisions to ratify (binding for remediation)

| ID | Decision |
|---|---|
| D1 | **Prefix identity or one call.** If CC-04 can guarantee token-prefix identity on the supported providers, keep the two-call pipeline. If it cannot, collapse to one LLM call and delete the KV argument from the docs. Do not keep two calls justified by a mechanism that does not hold. |
| D2 | **Indices are grounded by a manifest, not by annotating the conversation.** The pre-pass moves to the apply side; the classify instruction carries the index legend. |
| D3 | **The protected tail is a token budget**, with `keep_last_prompts` demoted to a maximum. |
| D4 | **The archive is prose with a cap**, not a growing index of drops. |
| D5 | **Deprecation is name-keyed** and documented, or deleted. No third path. |
| D6 | **"Not measured" is not green.** CC-08 must land before the epic can close; a compaction change without a ratio/retention measurement is not done. |

---

## 8. Remediation map

Thirteen independently executable tasks. Priority is impact/severity; urgency is
when to start. GitHub holds the live state; this table is the static plan.

| ID | Issue | Task | Priority | Urgency | Depends on | Primary files |
|---|---|---|---|---|---|---|
| CC-01 | [#417](https://github.com/jtrefon/amber/issues/417) | Pruning must not erase the active turn's evidence | P0 | now | — | `compressor_apply.cpp` |
| CC-02 | [#418](https://github.com/jtrefon/amber/issues/418) | Budget passes preserve pairing + the active task (and their own budget) | P0 | now | — | `compressor_apply.cpp` |
| CC-03 | [#419](https://github.com/jtrefon/amber/issues/419) | Fence stripping must not rewrite JSON escapes | P0 | now | — | `compressor_parser.cpp` |
| CC-04 | [#420](https://github.com/jtrefon/amber/issues/420) | Classify the live prefix (tools + unmodified snapshot) or drop the KV claim | P1 | now | D1 | `compressor.cpp`, `agent.cpp`, `compressor_request.cpp` |
| CC-05 | [#421](https://github.com/jtrefon/amber/issues/421) | Protected tail as a token budget; monotonic invariant | P1 | next | CC-01, CC-02 | `compressor_apply.cpp`, `config.*` |
| CC-06 | [#422](https://github.com/jtrefon/amber/issues/422) | Cap and summarise the archive; delete write-only payload | P1 | next | CC-02 | `compressor_apply.cpp` |
| CC-07 | [#423](https://github.com/jtrefon/amber/issues/423) | Ground classifier indices + make the archive's wire position explicit | P1 | next | CC-04 (or lands with it) | `compressor_request.cpp`, `dialect_openai.cpp` |
| CC-08 | [#424](https://github.com/jtrefon/amber/issues/424) | Measure ratio and retention; replace the stale canned scenario | P1 | next | — | `bench/`, `tests/` |
| CC-09 | [#425](https://github.com/jtrefon/amber/issues/425) | One budget relation: headroom below trigger | P2 | next | CC-05 | `compressor.cpp`, `compressor_apply.cpp` |
| CC-10 | [#426](https://github.com/jtrefon/amber/issues/426) | Gate guards count messages/calls; add a saturation bypass | P2 | next | — | `compressor.cpp` |
| CC-11 | [#427](https://github.com/jtrefon/amber/issues/427) | Knowledge deprecation reachable, or deleted | P2 | next | D5 | `compressor_parser.cpp`, `compressor_apply.cpp`, `compressor_request.cpp` |
| CC-12 | [#428](https://github.com/jtrefon/amber/issues/428) | Reconcile the three docs with the code | P2 | now | — | `docs/**` (done: `compression-pipeline.md`, `context-compression.md`, `loop-collapse.md`, `turn-classification.md`, `INDEX.md`, ownership claim) |
| CC-13 | [#429](https://github.com/jtrefon/amber/issues/429) | Module linkage, const-correctness and copy hygiene | P3 | later | all above | `compressor*.{h,cpp}` |
| CC-14 | [#439](https://github.com/jtrefon/amber/issues/439) | Compact inside a single active turn (pinned set; deterministic pass; consumption re-arm) | P2 | done | CC-05 | `compressor_apply.cpp`, `compressor.cpp` |
| CC-15 | [#464](https://github.com/jtrefon/amber/issues/464) | Replay + adversarial acceptance for context shaping | P1 | done | CC-14 | `bench/shape_replay.*`, `tests/` |
| CC-16 | [#465](https://github.com/jtrefon/amber/issues/465) | Every digested result recallable by id | P1 | done | — | `artifacts.cpp`, `compressor_apply.cpp` |
| CC-17 | [#466](https://github.com/jtrefon/amber/issues/466) | Project bulky bash output at the source (8 KiB, head+tail+handle) | P2 | done | CC-15 | `dispatch.cpp` |
| CC-18 | [#467](https://github.com/jtrefon/amber/issues/467) | Deterministic supersession at rebuild | P3 | deferred | CC-15 | `compressor_apply.cpp` |

### CC-18: deferred on measurement, not on opinion

The replay harness (`amber-bench shape-replay`) was run over 24 recorded
sessions (~485k tokens). Per rule, at a 32 KiB window:

| Rule | Tokens saved | Replaced | Referenced later |
|---|---:|---:|---:|
| in-turn (shipped) | 7% | 36 | 17 |
| bash projection (shipped) | 3% | 7 | 1 |
| supersede (CC-18) | **0%** | **0** | 0 |
| drop-all (control) | 83% | 293 | 140 |

No duplicate-read supersession existed in the corpus, so CC-18 buys nothing on
real data; it stays deferred with that number rather than shipping an unmeasured
rule. The control rule is what makes the table trustworthy: a shaper that drops
everything is flagged on 140 of 293 replacements.

### Findings → tasks

| Finding | Tasks |
|---|---|
| C1 | CC-04 (+D1) |
| C2 | CC-01 |
| C3 | CC-02 (+CC-05 for the tail) |
| H1 | CC-05 |
| H2 | CC-06 |
| H3 | CC-03 |
| H4 | CC-09 |
| H5 | CC-11 |
| M1 | CC-06 |
| M2 | CC-10 |
| M3 | CC-05 |
| M4 | CC-12 |
| M5 | CC-13 |
| M6 | CC-08 |
| Classification contract | CC-07 (+D2) |

---

## 9. Parallel execution plan

File ownership is the constraint, not task size. Shared files are the hazard:
`compressor_apply.cpp` carries seven tasks.

### Wave 1 — start immediately (disjoint)

| Session | Tasks | Owns |
|---|---|---|
| W1 | CC-01 + CC-02 | `compressor_apply.cpp` (prune + budget passes). **One session**: both are P0 and touch the same helpers. Land CC-01 first, then CC-02 rebases. |
| W2 | CC-03 | `compressor_parser.cpp` only |
| W3 | CC-12 | `docs/` only |
| W4 | CC-08 | `bench/` + new test file only |
| W5 | CC-07 (design phase) | produces the manifest spec; implementation waits for CC-04 |
| W6 | CC-10 | `compressor.cpp` gate class only (no pipeline region) |

### Wave 2 — after Wave 1 merges

| Session | Tasks | Notes |
|---|---|---|
| W7 | CC-04 | `compressor.cpp` + `agent.cpp`; rebase on W6 (gate region untouched) |
| W8 | CC-05 | rebase on W1; introduces the tail helper the later tasks use |
| W9 | CC-06 | rebase on W1/W8; archive write path |
| W10 | CC-09 | rebase on W8; one relation constant |
| W11 | CC-11 | small; rebase on W1 (apply) and W2 (parser) |

### Wave 3 — last

| Session | Tasks | Notes |
|---|---|---|
| W12 | CC-13 | touches every file in the module; must be the final refactor, after behaviour is settled |

**Rules for parallel sessions** (also in each issue):

1. One task per branch, `<type>/cc-NN-<slug>`.
2. Never edit a file another open task owns without saying so on the issue.
3. `compress()`'s signature and `CompressionConfig` are shared: announce changes
   in the epic before pushing.
4. Every task lands with its red test committed first (AGENTS.md).
5. Do not "fix forward" into a neighbouring finding; file it under the owning
   task instead.

---

## 10. Test plan

Each is a red test first; names are proposed, not final:

| Test | Covers | Currently fails |
|---|---|---|
| `compression_preserves_tool_pairing_across_budget_sweep` | C3/I2 | yes |
| `compression_never_drops_the_active_user_message` | C3/I3 | yes |
| `compression_budget_is_measured_after_the_archive_is_written` | I5 | yes |
| `prune_tool_io_keeps_the_last_tool_result_of_the_active_turn` | C2/I4 | yes |
| `compression_is_monotonic_on_a_single_prompt_session` | H1/M3/I1 | yes |
| `classify_request_is_a_token_prefix_of_the_last_wire_request` | C1/I7 | yes |
| `parser_survives_escaped_newlines_in_a_fenced_reply` | H3 | yes |
| `archive_entries_are_capped_batched_and_summarised` | H2/I8 | yes |
| `headroom_budget_is_below_the_trigger_threshold` | H4/I6 | yes |
| `gate_ignores_the_turn_floor_near_saturation` | M2 | yes |
| `deprecate_op_reaches_the_store_by_name` (or its deletion) | H5 | yes |
| `classifier_indices_are_grounded_in_a_manifest` | CC-07 | new |

Every budget test must use **multi-call tool groups**, not `user`/`assistant`
pairs. The existing suite's uniform two-role histories are why C3 was invisible.

---

## Appendix A — probe results

Probes were throwaway programs linked against `libagent_core.a`; they were not
committed. Reproduce by rebuilding the same synthetic history and calling the
public `agent::` helpers. Summary:

**P1 — classification is a no-op below the prompt guard** (`keep_last_prompts=10`):

```
prompts= 1 msgs  42 -> 43 | tokens    9675 ->    9717 | target 26214
prompts= 3 msgs 124 -> 125 | tokens   29011 ->   29053 | target 26214
prompts=12 msgs 493 -> 412 | tokens  116023 ->   96729 | target 26214
```

**P2 — the archive dominates the target** (`context_size=8192`, `target_pct=10`):

```
after apply_classification:  64 msgs, 11459 tokens
after enforce_target_budget: 34 msgs,  6418 tokens (target 409)
after enforce_headroom:      15 msgs,  3350 tokens (budget 3072)   ← over its own budget
pass 1: archive entries=38, archive message=762 tokens of 6573 final
```

**P3/P4 — tool pairing after the budget passes** (10-round tool session):

```
headroom sweep: 5/50 budgets produce an orphan tool result
target sweep:   3/39 budgets produce an orphan tool result
```

**P5 — active task dropped when the tail exceeds the headroom budget:**

```
in:  [system][user a][archive][assistant][user "THE ACTIVE TASK" 4000 chars]
enforce_headroom(window=900, budget=675)
out: [system "S"][system archive]            ← zero user messages
```

**P6 — prefill share of the pre-pass** (see [Performance](#4-performance)).

**P7 — parser corruption:**

```
fenced with escaped newline -> summary="line oneline two"   (silent)
bare with escaped newline   -> summary="line one\nline two" (correct)
```

**P8 — external linkage audit:**

```
nm -C lib/compressor_parser.o | grep ' T '
agent::parse_brief, agent::string_list, agent::parse_segment,
agent::parse_turn_range, agent::parse_knowledge_op,
agent::parse_knowledge_ops, agent::parse_response_body
```

---

## Appendix B — documents that must change

| Document | Change |
|---|---|
| `docs/spec/compression/compression-pipeline.md` | Remove `kFallbackContextBudget`; correct the "no mock LLM"/"headroom not implemented" gaps; state the real pre-pass and budget order; align invariants 1 and CP-11; describe the pure pipeline (not push/pop). |
| `docs/architecture/context-compression.md` | Replace the push/pop and gate-timing sections; delete `enforce_minimum_context`/`split_classify_response`; state the headroom/trigger relation; add the manifest. |
| `docs/deep-audit/context-system-analysis.md` | Its "resolved" note predates this review; link here for the open findings. |
| `docs/spec/INDEX.md` | Point `compression/*` at the reconciled specs and this review. |
| `docs/spec/context/context-ownership-and-parallel-compression.md` | The "two LLM calls share a KV prefix by content identity" claim must be qualified (it is true of classify→extract only). |
| This review | Update the remediation table with issue numbers as they are filed. |

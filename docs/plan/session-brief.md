# Session Brief — harness-maintained narrative that survives compression

Status: proposal (no code changes yet). Awaiting sign-off before RED.
Scope: core library (`lib/` + `include/agent/`). No UI changes in v1.
Depends on: P5 capped compression budget (`kMaxCompressBudget = 32'000`)
being in effect so the first compression — and thus the first brief —
arrives before late-run degradation. P5 is independent and shippable
first; the brief design is correct regardless of the gate, but its value
peaks only when the gate fires on time.

## 1. The problem

Compression preserves *turns* and *facts*, not *narrative coherence*. After a
compression cycle the model has fragments — a work-state summary, archived
one-liners, the last few verbatim turns — but no coherent "here is where we
are in the pursuit of the goal, why we chose this path, what we already ruled
out." The model has to reconstruct the arc from fragments, and that is exactly
where it loses the plot and starts re-reading files and re-walking dead ends.

Concretely, what compression destroys today:

- **Intent** — the *why* of the whole session. Lives in early user turns that
  get archived to one-liners or pruned.
- **Direction** — the chosen strategy + key decisions. Scattered across turns,
  partially in core but not consolidated.
- **Results log** — what was tried and what happened (worked / failed). Dead
  ends get pruned.
- **Ruled-out approaches** — the most valuable thing to preserve, and the
  thing compression is most aggressive about dropping (dead ends are the
  definition of "prune"). Post-compression, the model re-walks the same dead
  end.

Existing mechanisms do not cover this gap:

- `todowrite` captures *task steps* but not intent / rationale / dead-ends. It
  is also feature-flagged off (P1 found 0 adoption on non-RL-trained models).
- Memories capture *durable cross-session facts*, not *this session's arc*.
- The compressed-context `summary` field is an ephemeral work-state paragraph
  from the classify step — lives in the compressed-context message, gets
  archived on the next compression. It is "what just happened," not "where are
  we in the whole pursuit."

None of the three is a living session brief.

## 2. The design (decisions, with rationale)

### 2.1 Harness-maintained, not model-maintained

The brief is extracted by the compression pipeline, not maintained by the
model via a tool.

**Rationale:** the P1 experiment (`docs/plan/agentic-fix-plan.md` §6a) found 0
adoption of the `todowrite` tool across all runs on free-tier / 27B models —
they are not RL-trained tool users. A model-maintained brief hits the same
adoption wall. The compression pipeline already runs an LLM over the full
context (classify + extract); that is the ideal moment to distill the arc, and
it works for every model regardless of training. This inverts the P1 lesson:
bet on the harness maintaining state at the moment it already has the full
context in hand, not on the model maintaining state unprompted.

### 2.2 Re-injected via a dedicated message slot, never into the system prompt

The brief is injected into `prompt_copy` (the local copy of the context built
in `chat_once`) as a separate system message slot, after the system prompt —
exactly like memories (`lib/agent.cpp:244-263`) and skills
(`lib/agent.cpp:290-308`).

**Rationale:** this is the existing pattern for host-owned state that must
reach the model without touching the live context. The system prompt in the
live context (pushed once at `lib/agent.cpp:202`) is never modified. The live
context's hash chain is never touched by brief injection.

### 2.3 Seeded on first compression

The brief does not exist until the first compression cycle. The first
compression's extract pass seeds it from the full context; every subsequent
compression refreshes it.

**Rationale:** the pre-compression phase is where the full context is intact
and the model least needs a spine. The brief's value peaks after compression,
when the context is fragmented — and that is exactly when it appears. The
real risk in the pre-compression phase is not the missing brief but the gate
firing too late (P5); that is a gate problem, not a brief design problem, and
is handled by P5's capped budget.

### 2.4 Persistent across restarts

The brief store is filesystem-backed, like `MemoryStore`. It survives a
process restart; a stale brief from a previous session is cleaned up when a
new session begins.

**Rationale:** lets a session's arc resume after a crash or restart. More
surface area than in-memory (load / save, stale-brief cleanup) but matches
the `MemoryStore` pattern and the value is real for long sessions.

### 2.5 Done log: cap + summarize older

The Done log keeps the last ~10 entries verbatim; older entries are folded into
a single `earlier:` one-liner. The extract LLM does this collapse in the same
pass that refreshes the brief.

**Rationale:** bounded size preserves the arc without the brief itself becoming
the thing we are trying to shrink. The recent arc stays high-fidelity (last 10
verbatim); only the tail is summarized.

### 2.6 Coexists with todowrite, separate owners

The brief's Next field is the harness-maintained forward plan; todowrite stays
as the optional model-maintained checklist. When todowrite is off (default),
the brief's Next is the only forward plan that survives compression.

**Rationale:** different owners, no conflict. The brief gives the P1 value
(reliable forward plan across compression) without the P1 adoption wall.

## 3. The brief shape

Five fields, hard ~2 KB cap. Structured, not free-form prose (prose bloats and
gets skimmed).

```
[session-brief]
intent: <1-3 sentences — what the user asked for and why>
direction: <chosen approach + key constraints/decisions>
done:
  - <step> → <one-line result: worked/failed>
  - ...
  earlier: <one-line summary of older completed work>
next: <immediate next steps + rationale>
avoid:
  - <ruled-out approach> → <why it failed>
[/session-brief]
```

The `avoid` field is the differentiator that makes this worth building. Nothing
else in the harness preserves ruled-out approaches, and compression is most
aggressive about dropping exactly those turns. Post-compression, the model
re-walking a dead end is the canonical failure mode this fixes.

## 4. Architecture (all in core — `lib/` + `include/agent/`)

| Component | Location | Mirrors |
|---|---|---|
| `SessionBrief` struct + `SessionBriefStore` | `include/agent/session_brief.h`, `lib/session_brief.cpp` | `MemoryStore` (filesystem-backed, persists across restart) |
| Extract target added to compression step 2 | `lib/compressor_request.cpp` (`build_extract_request`), `lib/compressor_parser.cpp`, `lib/compressor_apply.cpp` | existing memory / skill extraction |
| Re-injection as dedicated message slot | `lib/agent.cpp` (`chat_once`, where memories inject today) | memory injection slot |
| Brief merge logic (append / prune / replace per field) | `lib/session_brief.cpp` | — |

### 4.1 `SessionBriefStore` (host-owned, like `MemoryStore`)

```cpp
struct SessionBrief {
    std::string intent;
    std::string direction;
    std::vector<std::string> done;      // last ~10 verbatim, newest last
    std::string earlier;                // one-line summary of older done
    std::string next;
    std::vector<std::string> avoid;     // append-only, capped
};

class SessionBriefStore {
public:
    void load(const std::string& path);
    void save(const std::string& path) const;
    // Merge a freshly-extracted brief into the store (per-field semantics).
    void merge(const SessionBrief& fresh);
    // Render the brief as the [session-brief]...[/session-brief] block.
    std::string render() const;
    bool empty() const noexcept;
    void clear() noexcept;
private:
    SessionBrief brief_;
    std::string path_;
};
```

Thread safety: single ownership, same as `MemoryStore` and `TodoStore`. Owned
by the agent thread; updated during compression (agent thread, synchronous),
read during `chat_once` (agent thread). No mutex.

### 4.2 Extract step gains a third target

The extract request (`build_extract_request`) gains a `brief` key alongside
`memories` and `skills`. The extract prompt receives the *existing* brief as
text (within the existing extract extension — zero added KV) so the LLM does a
merge, not a rewrite:

```json
{
  "memories": [...],
  "skills": [...],
  "brief": {
    "intent": "...",
    "direction": "...",
    "done": ["step → result", ...],
    "next": "...",
    "avoid": ["ruled-out → why", ...]
  }
}
```

Parse each section independently. If `brief` is missing or malformed,
memories / skills still apply — and the brief store retains its last good
state (no overwrite on parse failure). This follows the existing CP-08
pattern (extraction failure is non-fatal, classification result still used).

### 4.3 Merge semantics per compression cycle

The extract LLM sees the existing brief + the turns being compressed, then:

| Field | Merge | Rationale |
|---|---|---|
| Intent | Replace | Stable; the LLM refines it as understanding deepens |
| Direction | Replace | The chosen approach may shift; latest is truth |
| Done | Append new, cap + summarize older | New entries from this cycle's compressed turns; keep last ~10 verbatim; fold older into `earlier:` (extend, do not regenerate) |
| Next | Replace | Current forward plan; stale next-steps are noise |
| Avoid | Append only, cap | Dead ends never become valid; this list only grows; drop oldest if it hits the cap |

The `earlier:` line is carried forward and appended to, not re-summarized from
scratch. This makes the collapse a low-risk append on a single line, not a
full re-summarization of the session history.

### 4.4 Re-injection (the KV-critical detail)

The brief is injected into `prompt_copy` after the system prompt. Ordering
within the injected stack matters for KV reuse. Memories and skills change
rarely (promoted on compression, decay slowly). The brief changes on every
compression — more frequent.

**Order: `system → memories → skills → brief → conversation`.**

The brief is injected last in the injected stack, right before the
conversation. A brief change invalidates KV only for `[conversation]`, which
re-prefills every turn regardless. The stable memories / skills prefix keeps
its KV across brief changes. This maximizes KV reuse for the stable prefix.

## 5. How every architectural invariant is preserved

The brief never lives in `Context`. It lives in `SessionBriefStore` (host-owned)
and is injected into `prompt_copy` on every `chat_once` — the memory pattern.
This is the load-bearing decision: the brief interacts with none of the context
invariants because it is not in the context.

### 5.1 Hash-chain integrity (no in-place mutation)

`Context` (context.h:57-148) seals messages on `push()` and verifies the FNV-1a
chain on `get_all()`. The only sanctioned mutation is `clear() + push()`
(the compression rebuild, agent.cpp:442-444).

The brief adds zero new mutation paths. It lives in `SessionBriefStore`.
Re-injection copies it into `prompt_copy` — a local `std::vector<Message>`
discarded after the LLM call. The live `context_` is never touched by brief
injection. The only context mutation remains the existing `clear() + push()`
rebuild, which the brief does not modify.

### 5.2 KV cache reuse (no full prefill trigger)

The compression pipeline's two LLM calls share a KV prefix because the extract
request replays the classify request (compressor.cpp:332-341). Adding the brief
to the extract step adds ~500 tokens of text to the extract prompt — a cheap KV
extension, not a prefix change. The classify → extract KV sharing is preserved
because the replay structure is unchanged; the brief is additional content
*within* the extract request, not a new message before it.

For per-turn re-injection: the brief is injected last in the injected stack
(§4.4), so a brief change invalidates KV only for `[conversation]`, which
re-prefills every turn regardless. The stable memories / skills prefix keeps
its KV.

### 5.3 Pure compression pipeline

The pipeline reads `context_.get_all()` into a working copy and never mutates
the live deque (compressor.cpp:237-239). The brief extraction happens in the
extract step, which works on the *request* (working copy + classify pair), not
on the live context. The brief result is parsed and applied to
`SessionBriefStore` — a separate store, not the context. The only context
mutation remains the existing `clear() + push()` rebuild. The pipeline stays
pure.

### 5.4 Thread safety (single ownership)

`SessionBriefStore` is owned by the agent thread, same as `MemoryStore` and
`TodoStore`. Updated during compression (agent thread, synchronous), read during
`chat_once` (agent thread). No cross-thread access. Persistence
(filesystem-backed) follows `MemoryStore`'s pattern: save on the agent thread
after compression, load at construction. No mutex — single ownership.

### 5.5 System prompt never changes

The brief is injected as a separate system message slot in `prompt_copy`,
after the system prompt — exactly like memories (agent.cpp:252-261) and skills
(agent.cpp:290-308). The system prompt in the live context (pushed once at
agent.cpp:202) is never modified.

## 6. Solutions for each identified issue

### 6.1 Extract prompt complexity (third schema → parse-failure surface)

**Problem:** adding a 5-field brief to the extract JSON (currently memories +
skills) increases parse complexity. One bad field could kill all extraction.

**Solution:** same extract call, independent per-section parse, non-fatal
fallback. The extract response gains a `brief` key alongside `memories` and
`skills`. Parse each section independently. If `brief` is missing or malformed,
memories / skills still apply — and the brief store retains its last good
state (no overwrite on parse failure). This follows the existing CP-08 pattern.
Zero added LLM cost, zero KV impact, graceful degradation per section.

**Rejected alternative:** a third LLM call for brief extraction. This breaks
the KV-sharing invariant — a third call needs its own prefix or a second
replay, adding prefill cost. Not worth it for ~500 tokens of additional
extraction in the existing call.

### 6.2 Done-log collapse quality (LLM folding older entries into "earlier:")

**Problem:** the merge step folds older Done entries into a one-line `earlier:`
summary. A bad summary degrades the arc.

**Solution:** the extract LLM does an append-only merge, not a rewrite. The
extract prompt receives the existing brief (including the current Done list and
`earlier:` line) and is instructed to: keep the last 10 Done entries verbatim,
append new entries from this cycle, and *extend* the existing `earlier:` line —
not regenerate it. The `earlier:` line is carried forward and appended to, not
re-summarized from scratch. If the LLM fails to produce a good `earlier:`, the
fallback is to keep the existing one (non-fatal — the store retains its last
good state).

This makes the collapse a low-risk append operation on a single line, not a
full re-summarization of the session history.

### 6.3 Pre-compression gap (no brief until first compression)

**Problem:** before the first compression, no brief exists. The long
pre-compression phase has no spine.

**Solution:** accept the gap for v1 — it is where the brief is least needed.
The pre-compression phase is where the full context is intact; the model has
all turns verbatim and least needs a spine. The brief's value peaks after
compression, when the context is fragmented — and that is exactly when it
appears.

The real risk is not the gap itself but the gate firing too late (P5 found the
gate fires at ~131k tokens with default config, effectively never). The brief
and P5 are complementary: ship the brief, and ensure P5's capped budget
(`kMaxCompressBudget = 32'000`) is in effect so the first compression — and
thus the first brief — arrives before degradation. The brief design is correct
to seed on first compression; the *gate* must fire on time. Flagged as a
dependency, not a brief design flaw.

**Stretch (not required for v1):** a C++-side seed from the first user message
— extract `intent` only (the raw first user prompt), no LLM call. This gives a
minimal brief from turn 1 at zero cost. Hold for v2 if benchmark shows the
pre-compression phase needs it.

## 7. Relationship to existing pieces

- **todowrite**: coexists, separate owners. Brief's Next is harness-maintained;
  todowrite is the optional model-maintained checklist. When todowrite is off
  (default), the brief's Next is the only forward plan surviving compression —
  this gives the P1 value without the adoption wall. No conflict.
- **memories**: different scope. Memories = durable cross-session facts. Brief
  = this session's arc. The extract step tells the LLM which is which.
- **compressed-context `summary`**: not redundant — different temporal scopes.
  The summary is an ephemeral work-state paragraph from the classify step (lives
  in the compressed-context message, gets archived on next compression). The
  brief is structured intent / direction / done / next / avoid from the extract
  step (lives in the store, re-injected every turn). The summary is "what just
  happened," the brief is "where are we in the whole pursuit." Keep both.

## 8. RED test plan (TDD, per workflow)

Hermetic tests in `tests/run_tests.cpp` + `tests/agent_loop_test.cpp`, using
`FakeLLMClient` where an LLM call is needed.

1. `session_brief_store_load_save` — store round-trips to disk; persists across
   a simulated restart.
2. `session_brief_merge_done_append_and_cap` — merge appends new Done entries,
   caps at 10, folds older into `earlier:`.
3. `session_brief_merge_avoid_append_only` — Avoid only grows; never replaces;
   drops oldest when capped.
4. `session_brief_merge_intent_replace` — Intent / Direction / Next replace on
   merge.
5. `session_brief_extracted_on_compression` (hermetic, FakeLLMClient) —
   scripted compression returns brief JSON in the extract response; store
   updated; brief re-injected into the rebuilt context as a `[session-brief]`
   message.
6. `session_brief_survives_compression` (hermetic) — seed a brief, run
   compression, assert the brief message is present in the post-compression
   context and matches the store.
7. `session_brief_size_cap` — brief exceeding ~2 KB is truncated; re-injection
   stays cheap.
8. `session_brief_parse_failure_nonfatal` — malformed brief JSON in extract
   response does not affect memories / skills; brief store retains last good
   state.

## 9. Benchmark gate

New long-horizon scenario (suite `compression`, hermetic): a multi-step task
where compression fires mid-run and the model would otherwise lose the arc.
KPI: post-compression, `redundant` ↓ (less re-reading / re-trying dead ends)
and final-answer correctness holds. Before / after rows in `BENCHMARK.md` on
the baseline models (Qwen3.6-27B dense + Laguna S 2.1).

## 10. Open risks

- **Extract prompt complexity** — adding a third schema to the extract step
  increases parse-failure surface. Mitigation: independent per-section parse,
  non-fatal fallback (§6.1).
- **Done-log collapse quality** — the LLM folding older Done entries into
  `earlier:` is a small summarization task; if it is bad, the arc degrades.
  Mitigation: keep last 10 verbatim (recent arc is high-fidelity); only the
  tail is summarized; `earlier:` is extended, not regenerated (§6.2).
- **Pre-compression gap** — before first compression, no brief. Accepted for
  v1; revisit if benchmark shows the long pre-compression phase is where
  coherence breaks (§6.3).
- **Gate timing dependency** — the brief's value peaks only when the gate
  fires on time. P5's capped budget must be in effect. Flagged as a
  dependency, not a brief design flaw (§6.3).

## 11. Sequencing

Per the repo's Red → Proposal → Sign-off → Green → PR workflow:

1. **This doc** — PROPOSAL (target state, no code).
2. **Sign-off** — reviewer approves the architecture before any production
   code.
3. **RED** — write the failing tests (§8), commit on the feature branch so CI
   shows red.
4. **GREEN** — implement `SessionBriefStore`, the extract target, re-injection,
   and merge logic. Tests pass.
5. **Lint / analyze clean** — `make lint` + `make analyze` clean before commit.
6. **Benchmark** — before / after rows in `BENCHMARK.md` (§9).
7. **PR** — one change per PR, squash-merge to `main`.

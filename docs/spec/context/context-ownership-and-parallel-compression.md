# Spec: Context Ownership & Event-Driven Compression

## Status: Implemented (Phases 1-4 complete)

## Purpose

Two goals, one architectural stance:

1. **Protect the `Context` deque design as inviolable** — the sealed-message,
   hash-chained, `push`/`pop`/`clear`/`get_all`-only stack that made the agent
   stable. No in-structure locking, no copy-returning `get_all()`, no mutable
   handle. Document why, so it is never "simplified" away again.

2. **Formalize single ownership and make compression event-driven**: exactly one
   thread (the agent/compress worker) mutates a given `Context`; the UI
   interacts only through immutable snapshots taken when the owner is quiescent
   or via context/progress events. Compression runs on the background worker
   with a pure copy-based pipeline whose two LLM calls share a KV prefix by
   content identity, and the `CompressionObserver` progress events drive a live,
   decreasing context gauge.

This locks in the `Context` contract, documents the ownership model the
compression spec (`docs/spec/compression/compression-pipeline.md`) assumes, and
corrects that spec's stale push/pop description to the implemented
content-identity mechanism.

---

## Part 1 — The inviolable Context contract

### Why the deque design exists (do not regress this)

The context was historically mutated by random parts of the system. The
stack-like, immutable-by-design architecture is what made the agent stable:
every mutator collapsed onto the single `push`/`pop`/`clear` API and was then
eliminated. The design gives **integrity by construction**:

- Messages are **sealed on push** — no caller ever holds a mutable handle.
- `get_all()` returns a **`const std::deque<Message>&`** — read-only view.
- The FNV-1a hash chain (`get_all()` asserts `verify_chain()`) is the tripwire
  that catches any future in-place mutation.
- Compression and rebuild happen only via **full `clear()` + `push()` cycles** —
  never in-place edits.

### Hard rules (enforced in review, documented here as law)

1. **`Context` has NO mutex, NO copy-returning `get_all()`, NO `replace()`/`insert()`/
   `update()`/`set_message()`.** Adding any of these is a design regression.
2. **Thread safety is by ownership, not by locking.** Exactly ONE thread may
   mutate a given `Context` at a time. All other threads interact with it only
   through **immutable snapshots** (const-ref reads taken while the owner is
   quiescent) or **events**.
3. **The hash chain and `assert(verify_chain())` stay.** They are the integrity
   gate that made the design trustworthy.
4. **Any new code that needs to "edit" the context must do so by rebuild**
   (snapshot → transform the copy → `clear()` + `push()` the result).

### The single-mutator invariant

The `Agent` (or a dedicated owner thread driving it — see Part 2) is the **sole
mutator** of its `context_`. Concretely:

- The agent loop, compression, session load, and `/clear` all mutate `context_`
  **on the owner thread**.
- UI/session persistence reads a **snapshot** (`context().get_all()` → copy)
  only when the owner is not mid-mutation (quiescent / via event).
- No other subsystem reaches into `context_` directly.

---

## Part 2 — Context owner thread + event-driven progress

### Today's gap

- `Agent` owns `context_` but is invoked **synchronously** from the TUI router
  thread (`agent_worker`). Compression also runs on that same thread — so the
  UI is blocked during a (potentially 2-LLM-call) compression.
- `ContextEventSource` fires `(token_count, message_count)` on every
  push/pop/clear and the TUI already subscribes (`tui.cpp:285`). But because
  mutation happens on the router thread (shared with UI event handling), the
  event stream is not a reliable progress source.
- The v2 `EventBus` (13 typed events) exists but is largely unwired.

### Target model

Introduce a **context owner thread** — the single thread that may mutate a given
`Context`. The agent loop and all compression run **on this thread**. The TUI
and other consumers never touch `context_`; they subscribe to events.

```
┌────────────────────────────────────────────────────────────────┐
│ TUI (UI thread)                          Context owner thread  │
│                                                               │
│  subscribe ──► ContextEventSource ──► (tokens, msgs) events   │
│  subscribe ──► EventBus ──► typed lifecycle events            │
│                                                               │
│  render progress counter from events        Agent::run()      │
│  snapshot (quiescent) ──────────────────►   context_ (sole     │
│  / session save                            mutator: push/pop/  │
│                                             clear + emit)      │
└────────────────────────────────────────────────────────────────┘
```

Key properties:

1. **The owner thread is the only mutator.** `Context` needs no lock because
   there is never a concurrent writer. Readers take immutable snapshots when
   the owner is quiescent.
2. **The owner thread publishes events** on every mutation
   (`ContextEventSource`) and on lifecycle transitions (`EventBus`:
   `AgentTurnStart/End`, `ToolCallBefore/After`, `CompressionTriggered`, …).
3. **The TUI renders from events** — a smooth progress counter (tokens/messages
   decreasing during compression) without ever touching the deque.
4. **Session persistence** takes a snapshot via a thread-safe request/response
   to the owner (or reads a snapshot the owner publishes after each mutation),
   never by reaching into `context_` mid-flight.

### Event contract for progress

New/formalized events (extend `ContextEventSource` payloads and/or `EventBus`):

- `ContextMutated(tokens, msgs)` — after every push/pop/clear (exists today).
- `CompressionStarted(tokens_before, msgs_before)` — owner begins compression.
- `CompressionProgress(phase, tokens_remaining, msgs_remaining)` — emitted as
  the old chain is consumed / new chain assembled.
- `CompressionFinished(tokens_after, msgs_after, result)` — swap complete.
- `CompressionFailed(error)` — pipeline aborted, context untouched.

The TUI's context gauge becomes a pure subscriber: it shows `tokens_after`
after a `CompressionFinished`, and a monotonic decrease during
`CompressionProgress`.

---

## Part 3 — Compression: pure copy-based pipeline, event-driven progress

### The KV-reuse mechanism (implemented, not push/pop)

The two compression LLM calls share a KV prefix by **content identity**, not by
mutating the live deque:

- The classify request = `[post-collapse history] + [classify prompt]`.
- The extract request = `[same post-collapse history] + [classify prompt] +
  [classify response] + [extract prompt]` — the extract call's prefix is
  byte-identical to the classify call's, so the server extends the first call's
  KV cache (no full prefill between them).
- The pipeline is **pure**: it reads the live context into a working copy
  (`context.get_all()`), collapses/prunes/classifies on the copy, and never
  touches the live deque. The caller applies the result with one atomic
  `clear()` + `push()` on success. Invariant 7 holds by construction.

This is safer than the older push/pop design: message indices stay aligned
between the classify request and the apply pass (the LLM sees exactly the
post-collapse list that `apply_classification` consumes), and a failed pipeline
cannot leave a partial request on the stack.

### Where the parallelism actually is

The two compression LLM calls are **inherently serial** — the extract step
depends on the classify response — so "classify LLM ∥ assemble" is not safely
achievable. The parallelism that matters is structural:

1. **Compression runs on a background worker** (`Tui::compress_worker`), never
   on the UI thread — the interface stays responsive for the whole pipeline.
2. **The UI is event-driven.** `CompressionObserver` fires per-phase events
   (`on_compress_start`, `on_loop_collapse`, `on_llm_request_sent`,
   `on_llm_reply_received`, `on_parse_result`, `on_apply_result`,
   `on_progress`, `on_compress_done`), bridged to the host by
   `CompressionReporter` through `AgentHooks.on_status` →
   `AgentEvent::Status`/`CompressResult`. The context gauge ticks downward
   through the pipeline and lands on `tokens_after` at the swap.
3. **The swap is atomic and single-owner.** Only the worker (the owner) calls
   `clear()` + `push()`; the UI never touches the deque (Part 2 rules).

```
Background worker (sole owner)                 UI thread (subscriber)
─────────────────────────────                  ─────────────────────
snapshot → collapse/prune → on_progress
classify LLM call (KV ext #1) ──► status ──►   gauge ticks down
apply classification + headroom → on_progress
extract LLM call (KV ext #2) ────► status ──►   gauge ticks down
atomic clear()+push(new chain) ─► CompressResult ─► gauge = tokens_after
```

### What the new chain contains

1. **System prompt** (index 0, always preserved).
2. **Compressed-context archive message** — the summary JSON block
   (`{"type":"compressed_context","archive":[{turns,summary}],...}`), which is
   updated/accumulated across compressions.
3. **Kept-early messages** — the classifier's `core` turns from before the
   recent window that are still relevant (decisions, current-task facts).
4. **Recent tail** — the last two user turns and everything after them,
   verbatim (existing safety net; the active task is never dropped).
5. **Memories/skills** extracted by the second LLM call are applied to the
   store (existing `apply_compression_result`).

Tree-shaking (loop collapse + tool-output pruning) runs C++-side before the
classifier.

### Single-owner threading rules

1. The agent/compress worker is the only mutator of `context_` (Part 1 rule 2).
2. The pipeline operates only on the immutable copy — it never calls
   push/pop/clear on the live context.
3. The UI never snapshots a context the worker is mid-mutation on: manual
   `save_session()` defers while `busy()`; `autosave` fires only on quiescent
   `Done`; shutdown saves join the worker first.
4. Progress/status events are published by the worker and serialized on the UI
   thread (`drain_events`).

### Failure / rollback (unchanged guarantee)

If any LLM call or parse fails, the pipeline returns the ORIGINAL history
unchanged and the live context is untouched (spec invariant 7). The working
copy is discarded. Cooldown covers the attempt. The owner never swaps a partial
chain.

---

## Part 4 — Scope of work (proposed implementation order)

### Phase 1 — Lock in the pure copy-based pipeline (KV reuse verified)

The pipeline's KV-reuse mechanism is **content-identical prefixes on a pure
copy**, not live-context push/pop:

- The extract request replays the classify request as a prefix, so the second
  LLM call extends the first's KV cache (no full prefill between them).
- The pipeline reads the live context into a working copy and never mutates
  the deque — invariant 7 (failed compression leaves context untouched) holds
  by construction, and message indices stay aligned between the classify
  request and the apply pass.

- [x] Fix the misleading comment in `lib/agent.cpp` (`run_compression`) that
      claimed live-context push/pop.
- [x] Fix the stale test comment in `tests/agent_loop_test.cpp` describing the
      old push/pop behavior.
- [x] Add a hermetic test pinning the KV-reuse contract: the extract request
      shares a byte-identical prefix with the classify request.
- [x] Update `docs/spec/compression/compression-pipeline.md` to describe the
      content-identity mechanism instead of the stale push/pop description.

### Phase 2 — Context owner thread + event-driven progress

- [x] Make `ctx_used_` (`tui/tui.h`) an `std::atomic<long>` — the gauge is
      written by the agent/compress worker and read by the UI thread.
- [x] Guard manual `save_session()` against snapshotting a context the worker
      is mid-mutation on (defer when `busy()`); `autosave` (on_done) and
      shutdown saves are quiescent-safe by construction.
- [x] Document the single-owner rule at the `snapshot()`/autosave call sites.

### Phase 3 — Parallel two-thread compression

The two compression LLM calls are **inherently serial** (extract depends on
the classify response), so the user's envisioned "LLM call ∥ tree-shake"
parallelism is not safely achievable. The honest delivery:

- [x] Compression already runs on a background worker (`Tui::compress_worker`),
      so the UI thread stays responsive — no blocking prefill stall.
- [x] Add `CompressionObserver::on_progress(tokens, msgs)`, fired after
      collapse/prune and after apply+headroom, and forward it through
      `CompressionReporter` to the host — the context gauge now ticks downward
      through the pipeline instead of only jumping at the swap.
- [x] Verify the pipeline is pure (works on a copy; the live deque is touched
      only by the final `clear()`+`push()` on success).

### Phase 4 — Documentation & regression locks

- [x] AGENTS.md "Context stack architecture": add the single-owner rule (never
      a mutex / copy-returning `get_all()`), the pure-copy compression note,
      and the spec cross-reference.
- [x] `tests/build_hygiene.sh` P6: greps `context.h` and fails if a mutex,
      a copy-returning `get_all()`, or a mutation method is ever added.

---

## Out of scope (deferred)

- Wire the full v2 `EventBus` lifecycle (AgentTurnStart/End etc.) into the
  agent loop — only the compression progress events are in scope here.
- Bench/plugin work (already lowest priority).
- Slash-command work (already fixed).

## Cross-references

- Depends on: `docs/spec/compression/compression-pipeline.md` (the KV-reuse
  design this restores), `include/agent/context.h` (the deque contract),
  `lib/agent.cpp` (`run_compression`, `chat_once`).
- Depended on by: `tui/` (context gauge, session persistence), `AGENTS.md`
  (architecture invariants).

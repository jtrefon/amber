# Fix Proposal: Compression Pipeline Integration

- **Status:** Proposal — awaiting sign-off
- **Applies to:** `lib/agent.cpp`, `lib/compressor.cpp`, `lib/compressor_apply.cpp`, `lib/memory_store.cpp`, `lib/compressor_request.cpp`
- **Depends on:** None (all components already exist and are tested)
- **Design patterns:** Strategy, Observer, Memento, Command

---

## Problem Statement

The compression subsystem has a complete, tested pipeline — but `Agent::compress_now()` bypasses it entirely. The explicit `/compress` command performs brute-force message deletion with a placeholder summary, while the automatic gate in `chat_once()` uses the real pipeline. This creates three critical failures:

1. **No tree-shaking** — loop collapse, classification, and archive replacement never run on manual compression
2. **No memory/skill engine integration** — the LLM never extracts memories or skills during compression
3. **No decay/scoring engine** — evidence counts and freshness never update from compression events

The pipeline is designed for **single-GPU inference compliance**: one LLM call, same system prompt, KV cache preserved. `compress_now()` violates all three constraints.

---

## Current State (Broken)

### `compress_now()` — stub implementation

```cpp
// lib/agent.cpp:215-254 — what it actually does
CompressionResult Agent::compress_now() {
    if (!compression_ || context_.size() < 2) return {};
    
    size_t keep = cfg_.compression_min_turns > 0 ? cfg_.compression_min_turns : 10;
    size_t pop_count = context_.size() - keep - 1;
    
    context_.pop(pop_count);  // BRUTE-FORCE DELETE
    
    Message summary_msg;
    summary_msg.role = "system";
    summary_msg.content = "[compressed: " + std::to_string(pop_count) +
                          " earlier messages removed...]";  // PLACEHOLDER
    context_.push(std::move(summary_msg));
    
    return build_result();
}
```

### What the architecture specifies (docs/spec/compression/compression-pipeline.md)

```
compress_now() should:
  1. Pre-process: collapse_loops(history)        // tree-shake loops
  2. Build compression request message            // LLM instruction
  3. LLM call (same system prompt, no tools)      // classification
  4. Parse JSON → CompressionResponse             // core/context/prune tags
  5. Apply classification (prune/archive/core)    // structured compression
  6. Upsert memories/skills from LLM              // memory engine
  7. decay_all() + save()                         // scoring engine
```

### Gap analysis

| Spec Step | Current | Status |
|-----------|---------|--------|
| 1. collapse_loops | ❌ Not called | **MISSING** |
| 2. Build request | ❌ Not called | **MISSING** |
| 3. LLM classification | ❌ Not called | **MISSING** |
| 4. Parse response | ❌ Not called | **MISSING** |
| 5. Apply classification | ❌ Not called | **MISSING** |
| 6. Upsert memories | ❌ Not called | **MISSING** |
| 7. decay + save | ❌ Not called | **MISSING** |

Every step of the pipeline is bypassed.

---

## Root Cause Analysis

### RC1: `compress_now()` was never wired to the pipeline

The pipeline (`CompressionPipeline::compress()`) was built and tested. The gate in `chat_once()` was wired correctly:

```cpp
// lib/agent.cpp:147-154 — gate works correctly
if (gate_ && compression_) {
    if (gate_->should_compress(context_, cfg_)) {
        auto cc = load_compression_config(cfg_);
        auto compressed = compression_->compress(prompt_copy, cc, client_);
        prompt_copy.assign(compressed.begin(), compressed.end());
    }
}
```

But `compress_now()` was never updated to use the same path. It appears to be a legacy implementation that predates the pipeline refactor.

### RC2: Context immutability violation

The `Context` class is an **immutable stack** — messages are sealed on push, never modified. The pipeline operates on **copies** of the history:

```cpp
// CompressionPipeline::compress() — receives const reference, operates on copy
std::vector<Message> compress(
    const std::vector<Message>& history,  // immutable input
    const CompressionConfig& cfg,
    LLMClient& client,
    CompressionObserver* observer,
    CompressionResponse* response_out) override {
    auto copy = history;  // operates on copy
    // ... classification, LLM call, apply ...
    return result;  // returns NEW compressed history
}
```

`compress_now()` mutates the live stack directly via `context_.pop()` — a **direct mutation** that violates the immutability contract. If the LLM call fails mid-compression, the stack is corrupted with no rollback.

### RC3: Memory engine disconnected

The memory/skill engine exists and is functional:
- `MemoryStore` with evidence counting, promotion, decay
- `MemoryRetriever` for relevance-scoring retrieval
- `apply_memory_ops()` / `apply_skill_ops()` for upsert/deprecate from LLM classification
- Persistence to JSON file

But `compress_now()` never calls any of these. The `CompressionResponse` contains `memory_ops` and `skill_ops` that should be applied — they're just ignored.

### RC4: Tree-shaking is shallow — no semantic relevance assessment

**This is the deepest problem.** The current loop collapse (`collapse_loops()`) only detects identical tool-call sequences (3+ repetitions of the same tool with the same arguments). This catches *obvious* loops but misses the real fat:

**Real-world scenario:** Agent investigates 5 different bugs in a codebase:
1. Bug A: reads `foo.cpp` (200 lines), `bar.h` (100 lines), grep searches — **FIXED**
2. Bug B: reads `baz.cpp` (150 lines), `qux.h` (80 lines), runs tests — **FIXED**
3. Bug C: reads `main.cpp` (300 lines), `config.h` (50 lines) — **FIXED**
4. Bug D: reads `util.cpp` (120 lines), `helper.h` (60 lines) — **FIXED**
5. Bug E (current): reads `agent.cpp` (400 lines), `compressor.h` (100 lines) — **IN PROGRESS**

After 50+ turns, the context is ~75% dead investigation data (Bugs A-D). The current approach:
- **Loop collapse** — no-op (no identical tool calls, each file was read once)
- **Brute-force pop** — deletes the oldest turns (Bug A investigation), but Bug B-D's file reads are still in context wasting tokens
- **No classification** — the LLM never gets to decide what's relevant

**What should happen:**
1. Bugs A-D are **pruned** — entire investigation chains removed
2. One-line summary inserted: *"Bugs A-D in foo/bar/baz/qux were investigated and fixed"*
3. Bug E investigation is **core** — kept verbatim
4. Active task state (current file reads, grep results) is **core**
5. Dead-end reads (e.g., `helper.h` that turned out irrelevant) are **pruned**

The pipeline *can* do this — the LLM classification prompt asks for core/context/prune tags. But `compress_now()` never calls it.

---

## Target Architecture

### Principle: Pipeline is the single source of truth

Both the automatic gate (in `chat_once()`) and manual compression (in `compress_now()`) must use the **same pipeline**. No code duplication. The pipeline already handles:
- Loop collapse (tree-shaking)
- LLM classification (core/context/prune)
- Archive replacement with summaries
- Memory/skill extraction
- Observer pattern for status reporting
- Error handling (fallback to original history on LLM failure)

### Pipeline flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    compress_now()                                │
│                                                                 │
│  1. Snapshot before = context_.get_all()                         │
│     (immutable — never mutate live stack)                         │
│     tokens_before = context_.token_count()                        │
│                                                                 │
│  2. CompressionReporter reporter(hooks_, r)                      │
│     reporter.set_before(msgs_before, tokens_before)              │
│                                                                 │
│  3. auto cc = load_compression_config(cfg_)                      │
│     CompressionResponse cr;                                      │
│                                                                 │
│  4. compressed = compression_->compress(before, cc, client_,     │
│                                          &reporter, &cr)          │
│     ┌─────────────────────────────────────────────────────┐     │
│     │ Inside pipeline (already implemented & tested):      │     │
│     │   a. copy = before                                   │     │
│     │   b. collapse_loops(copy) — tree-shake loops         │     │
│     │   c. build_compression_request() — LLM instruction   │     │
│     │   d. client.chat(copy, {}) — single LLM call         │     │
│     │   e. parse_compression_response() → cr               │     │
│     │   f. apply_classification(copy, cr)                  │     │
│     │   g. return compressed history                       │     │
│     └─────────────────────────────────────────────────────┘     │
│                                                                 │
│  5. context_.clear()                                            │
│     for (auto& msg : compressed)                                 │
│         context_.push(std::move(msg))                            │
│     (atomic replace — if pipeline fails, original is returned)   │
│                                                                 │
│  6. if (memory_store_ && !cr.memory_ops.empty())                │
│         apply_memory_ops(*memory_store_, cr.memory_ops, ...)     │
│     if (!cr.skill_ops.empty())                                   │
│         apply_skill_ops(*memory_store_, cr.skill_ops, ...)       │
│     memory_store_->decay_all()                                   │
│     memory_store_->save(path)                                    │
│                                                                 │
│  7. r.messages_before = msgs_before                              │
│     r.messages_after = context_.size()                            │
│     r.tokens_before = tokens_before                               │
│     r.tokens_after = context_.token_count()                       │
│     last_compression_ = r                                        │
│     return r                                                     │
└─────────────────────────────────────────────────────────────────┘
```

### Key properties

1. **Immutable context** — operates on a copy. The live `context_` is only modified after the pipeline succeeds (atomic replace via `clear()` + `push()`).
2. **Single GPU** — uses `client_` (the same LLM client), same system prompt, same context. No KV cache invalidation.
3. **Memory engine connected** — upserts memories/skills from LLM classification, runs decay, persists to file.
4. **Error-safe** — if the LLM call fails, the pipeline returns the original history unchanged. The observer fires `on_error()`.
5. **Observer pattern** — `CompressionReporter` bridges pipeline events to `AgentHooks::on_status` for UI feedback.

### Semantic tree-shaking: how classification works

The pipeline sends the full history (post-loop-collapse) to the LLM with the classification prompt. The LLM returns a JSON with per-turn tags:

```
{"classification": [
    {"turns": "0-12", "tag": "prune", "summary": ""},           // Bug A fixed
    {"turns": "13-20", "tag": "prune", "summary": ""},          // Bug B fixed
    {"turns": "21-28", "tag": "prune", "summary": ""},          // Bug C fixed
    {"turns": "29-35", "tag": "prune", "summary": ""},          // Bug D fixed
    {"turns": "36-45", "tag": "core", "summary": ""},           // Bug E active
    {"turns": "46-48", "tag": "context", "summary": "tried grep for agent::run but irrelevant"}
]}
```

The `apply_classification()` function then:
- **prune** — removes entirely (Bugs A-D, dead-end grep)
- **core** — keeps verbatim (Bug E, active work)
- **context** — replaces with archive entry (grep that was tried but irrelevant — summary preserved for awareness)

This is **semantic tree-shaking**: the LLM assesses relevance based on the *current task context*, not just message age.

### Tree-shaking quality: why it matters

| Scenario | Without tree-shaking | With tree-shaking |
|----------|---------------------|-------------------|
| 5 bugs fixed, 1 in progress | 75% context wasted on dead investigations | 90%+ reclaimed |
| File read irrelevant to current task | Stays in context wasting tokens | Pruned or archived |
| Dead-end grep search | Stays in context | Pruned or archived with note |
| Active task state (current file) | Kept (good) | Kept as core |
| Bug fixed 10 turns ago | Kept (waste) | Pruned with one-line summary |

**The LLM is the only thing that can do this.** Heuristic-based approaches (age, token count, keyword matching) cannot distinguish between "investigated file that's still relevant" and "investigated file that's been superseded."

---

## Implementation Plan

### Phase 1: Wire `compress_now()` to the pipeline (Core)

**Files:** `lib/agent.cpp`

Replace the current stub with the pipeline integration:

```cpp
CompressionResult Agent::compress_now() {
    if (!compression_ || context_.size() < 2) return {};

    // Snapshot BEFORE compression — immutable, never mutate live stack
    auto before = context_.get_all();
    size_t msgs_before = before.size();
    size_t tokens_before = context_.token_count();

    CompressionResult r;
    CompressionReporter reporter(hooks_, r);
    reporter.set_before(msgs_before, tokens_before);

    // Load config and call pipeline
    auto cc = load_compression_config(cfg_);
    CompressionResponse cr;

    auto compressed = compression_->compress(
        std::vector<Message>(before.begin(), before.end()), // copy
        cc, client_, &reporter, &cr
    );

    // Replace context atomically — pipeline returns original on failure
    context_.clear();
    for (auto& msg : compressed)
        context_.push(std::move(msg));

    // Apply memory/skill ops from LLM classification
    if (memory_store_ && !experience_cfg_.store_path.empty()) {
        memory_store_->set_current_turn(turn_counter_);
        std::vector<ExtractionItem> items;

        if (!cr.memory_ops.empty())
            apply_memory_ops(*memory_store_, cr.memory_ops,
                            experience_cfg_.store_path, &items);
        if (!cr.skill_ops.empty())
            apply_skill_ops(*memory_store_, cr.skill_ops,
                           experience_cfg_.store_path, &items);

        memory_store_->decay_all();
        memory_store_->save(experience_cfg_.store_path);

        last_extraction_.items = std::move(items);
    }

    r.messages_before = msgs_before;
    r.messages_after = context_.size();
    r.tokens_before = tokens_before;
    r.tokens_after = context_.token_count();
    last_compression_ = r;
    return r;
}
```

**Verification:**
- [ ] `make clean && make && make test` passes
- [ ] Manual test: `/compress` in TUI with 20+ turn conversation — check that LLM classification is applied, not brute-force pop
- [ ] Manual test: `/compress` after tool loops — check that loops are collapsed
- [ ] Manual test: memory store file updated after compression

### Phase 2: Fix memory engine integration

**Files:** `lib/agent.cpp`, `lib/memory_store.cpp`

#### 2a: Use configured `decay_rate`

Current code always decrements by 1 regardless of config:

```cpp
// lib/memory_store.cpp:236-248
void decay_all() override {
    for (auto& [id, mem] : memories_) {
        if (mem.evidence_count > 0)
            mem.evidence_count -= 1;  // ignores decay_rate
```

Fix:

```cpp
void decay_all() override {
    for (auto& [id, mem] : memories_) {
        // Use configured decay rate (default 0.1), floor at 0
        mem.evidence_count = std::max(0,
            static_cast<int>(mem.evidence_count * (1.0 - cfg_.decay_rate)));
        if (mem.evidence_count <= 0)
            mem.promoted = false;
    }
    for (auto& [id, sk] : skills_) {
        sk.evidence_count = std::max(0,
            static_cast<int>(sk.evidence_count * (1.0 - cfg_.decay_rate)));
        if (sk.evidence_count <= 0)
            sk.promoted = false;
    }
}
```

**Verification:**
- [ ] `decay_all()` with `decay_rate = 0.1` reduces evidence by 10%, not by 1
- [ ] Items at 0 evidence set to `promoted = false`

#### 2b: Wire memory store to `compress_now()` (done in Phase 1)

Already included in Phase 1 implementation above.

### Phase 3: Fix context immutability in automatic gate

**Files:** `lib/agent.cpp`

The gate in `chat_once()` currently compresses a **copy** of the prompt (correct), but the result is only used for the current LLM call — it's never persisted back to `context_`. This means:
- Each LLM call recomputes compression from scratch
- The live `context_` grows unbounded until `compress_now()` is called
- KV cache is rebuilt every time because the system prompt never changes

**Current behavior:**
```cpp
// lib/agent.cpp:147-154
if (gate_ && compression_) {
    if (gate_->should_compress(context_, cfg_)) {
        auto cc = load_compression_config(cfg_);
        auto compressed = compression_->compress(prompt_copy, cc, client_);
        prompt_copy.assign(compressed.begin(), compressed.end());
        gate_->set_last_compress_turn(turn_counter_);
    }
}
```

**Target behavior:**
After the gate fires, the compressed result should replace `context_` so subsequent turns use the compressed context. This requires:
1. Gate fires → pipeline compresses → compressed history replaces `context_`
2. Cooldown prevents re-compression for N turns (already implemented in `is_within_cooldown()`)
3. KV cache preserved because system prompt at `context_[0]` is never modified

**Implementation:**

```cpp
// lib/agent.cpp:147-165 — updated gate logic
if (gate_ && compression_) {
    if (gate_->should_compress(context_, cfg_)) {
        auto cc = load_compression_config(cfg_);
        CompressionReporter reporter(hooks_);

        auto before = context_.get_all();
        auto copy = std::vector<Message>(before.begin(), before.end());

        auto compressed = compression_->compress(copy, cc, client_, &reporter);

        // Atomic replace — if pipeline fails, compressed == original
        context_.clear();
        for (auto& msg : compressed)
            context_.push(std::move(msg));

        gate_->set_last_compress_turn(turn_counter_);
    }
}
```

**Verification:**
- [ ] Gate fires → context_.size() decreases after LLM call
- [ ] Cooldown prevents re-compression for N turns
- [ ] System prompt at `context_[0]` is never modified

### Phase 4: Implement budget enforcement (Safety net)

**Files:** `lib/compressor_apply.cpp`

The architecture specifies:

> *"If the LLM output violates the budget (e.g. too many core turns), the C++ layer walks backward from oldest core turns, promoting them to context or prune until the budget fits. This is a hard C++ safety net."*

Budget:

| Budget | Fraction | Purpose |
|--------|----------|---------|
| core | 0.30 | Verbatim active turns |
| archive | 0.15 | Structured JSON context block |
| headroom | 0.50 | Model output space after compression |

**Implementation:**

```cpp
// lib/compressor_apply.cpp — after apply_classification()
std::vector<Message> enforce_budget(
    const std::vector<Message>& compressed,
    size_t context_size_tokens,
    const CompressionResult& r) {

    // core + archive must fit in 45% of context window
    size_t budget = context_size_tokens * 0.45;
    size_t used = r.tokens_after;

    if (used <= budget) return compressed;  // within budget

    // Walk backward from oldest core turns, promote to context/prune
    auto result = compressed;
    for (size_t i = 1; i < result.size(); ++i) {  // skip system prompt
        if (result[i].role == "system") continue;  // skip archive blocks

        // Promote to context (replace with summary)
        Message summary;
        summary.role = "system";
        summary.content = "[archived: turn " + std::to_string(i) + "]";
        result[i] = std::move(summary);

        // Recalculate token count
        size_t new_used = 0;
        for (const auto& msg : result)
            new_used += message_tokens(msg);

        if (new_used <= budget) break;  // within budget now
    }

    return result;
}
```

**Verification:**
- [ ] When LLM returns too many core turns, budget enforcement reduces them
- [ ] System prompt and archive blocks are never promoted
- [ ] Test with mocked LLM returning all-core classification

### Phase 5: Fix `compress_now()` stats reporting

**Files:** `lib/agent.cpp`

Current stats are incomplete:

```cpp
r.tokens_before = 0;  // never computed — lost after pop
r.messages_before = pop_count;  // wrong — should be context_.size() before pop
```

Fix (included in Phase 1 implementation):

```cpp
r.messages_before = msgs_before;     // captured before compression
r.messages_after = context_.size();
r.tokens_before = tokens_before;     // captured before compression
r.tokens_after = context_.token_count();
```

---

## What Each Phase Fixes

| Issue | Phase | Before | After |
|-------|-------|--------|-------|
| No tree-shaking | 1 | brute-force pop | collapse_loops + classification |
| No memory engine | 1 | memory store idle | upsert + decay + save |
| No scoring engine | 1 | evidence never updates | decay_all + save |
| Context mutation | 1 | `context_.pop()` | atomic replace |
| No KV cache preservation | 3 | gate recomputes every call | compressed context persisted |
| No budget enforcement | 4 | no safety net | backward walk from oldest |
| Wrong stats | 5 | `tokens_before = 0` | captured before compression |
| Ignored `decay_rate` | 2 | always -1 | uses configured rate |
| Shallow tree-shaking | 1 | no semantic assessment | LLM classifies relevance |

---

## Risk Assessment

### Low risk

- **Pipeline already tested** — all pipeline components have test coverage (see `tests/run_tests.cpp` lines 1555–1949). The pipeline is proven to work.
- **No API changes** — `compress_now()` signature unchanged, returns `CompressionResult`.
- **Error-safe** — pipeline returns original history on LLM failure. No data loss.

### Medium risk

- **LLM call cost** — `compress_now()` will now make an LLM call during manual compression. This is the intended behavior but adds latency. The automatic gate already does this — the only difference is that manual compression was free before.
- **Memory store path** — if `experience_cfg_.store_path` is empty, memory ops are silently skipped. This is the current behavior and is correct.

### Mitigation

- **Phase 1 only** — Phase 1 wires the pipeline. Phase 2-5 are incremental improvements that can be backported if needed.
- **Coarse error handling** — if the LLM call fails, the pipeline returns the original history. The user sees `compress_now()` return an empty result with `r.error` populated.

---

## Dependencies and Ordering

```
Phase 1 (wire pipeline) ────────────────────────────────────────────────┐
    Phase 2 (decay_rate fix) ──────────────────────────────────────────┤
        Phase 3 (gate persistence) ────────────────────────────────────┤
            Phase 4 (budget enforcement) ──────────────────────────────┤
                Phase 5 (stats fix) ───────────────────────────────────┘
```

**Phase 1 is the critical path** — everything else depends on it. Phases 2-5 can be done in parallel after Phase 1 is merged.

---

## Verification Checklist

### Build & test

- [ ] `make clean && make && make test` passes
- [ ] `make lint` clean (no new clang-tidy warnings)
- [ ] `make analyze` clean (no new cppcheck warnings)

### Functional

- [ ] `/compress` in TUI with 20+ turn conversation applies LLM classification
- [ ] `/compress` after tool loops collapses loops before classification
- [ ] Memory store file updated after compression
- [ ] Skills triggered by `trigger_phrase` after upsert
- [ ] `decay_rate` config respected (not hard-coded -1)
- [ ] Context immutability preserved (no direct mutation of `context_`)
- [ ] System prompt at `context_[0]` never modified during compression
- [ ] Stats report correct `tokens_before` / `tokens_after`

### Integration

- [ ] Automatic gate in `chat_once()` persists compressed context
- [ ] Cooldown prevents re-compression for N turns
- [ ] Two LLM calls after gate fires use same compressed context (KV cache preserved)
- [ ] Budget enforcement reduces oversized core classification

---

## Open Questions

### OQ1: Should `compress_now()` use a different model than the main agent loop?

**Current:** Both use `client_` (same model). This is correct for single-GPU compliance — no extra context load.

**Alternative:** Use a cheaper model for compression classification. This would reduce cost but requires a second LLM client, which means loading a second model on a single GPU — likely causing VRAM pressure and offload.

**Recommendation:** Same model. The compression LLM call is a single request, and the classification prompt is short. Cost impact is minimal.

### OQ2: Should automatic gate compression be destructive or non-destructive?

**Current:** Non-destructive — compresses a copy, uses it for the LLM call, discards it. `context_` grows unbounded.

**Alternative:** Destructive — gate fires, compresses, replaces `context_`. Cooldown prevents thrashing.

**Recommendation:** Destructive (Phase 3). Non-destructive means the context grows unbounded and the gate recomputes compression every call. The cooldown already prevents thrashing, and the pipeline is error-safe (returns original on failure).

### OQ3: Should memory/skill ops from compression be conditional?

**Current:** Always applied if `memory_store_` is non-null.

**Alternative:** Configurable — e.g., `cfg_.compression_extract_memories`.

**Recommendation:** Always applied. The LLM only extracts memories/skills when it identifies them in the classification response. If the response is empty, no ops are applied. No cost.

---

## References

- **Compression pipeline spec:** `docs/spec/compression/compression-pipeline.md`
- **Loop collapse spec:** `docs/spec/compression/loop-collapse.md`
- **Turn classification spec:** `docs/spec/compression/turn-classification.md`
- **Memory store spec:** `docs/spec/memory/memory-store.md`
- **Context compression architecture:** `docs/architecture/context-compression.md`
- **Issue register:** `docs/issues.md` (C1, C2, H1-H4 resolved)
- **Fix tracker:** `docs/fix-tracker.md` (FIX-004: compress_now dedup)
- **Test coverage:** `tests/run_tests.cpp` (lines 1555–1949, compression tests)
- **AGENTS.md:** Engineering principles, SOLID, size limits, TDD workflow

---

## Sign-off

| Role | Name | Date | Status |
|------|------|------|--------|
| Author | Frank | 2026-07-29 | ✍️ |
| Reviewer | | | ⬜ |
| Sign-off | | | ⬜ |

**Next step:** Reviewer approves → implement Phase 1 → test → merge.

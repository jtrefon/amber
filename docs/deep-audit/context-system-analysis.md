# amber Context System — Deep Architecture Analysis

**Author:** Frank  
**Date:** 2026-07-29  
**Last updated:** 2026-07-30  
**Method:** Code trace on every concept — no assumptions, no borrowed analysis

> **Note:** Items flagged as "BUG" or "gap" in this analysis have since been
> resolved. See the [Phase 0.2 Implementation Status](#7-phase-02-implementation-status)
> table at the end of this document for the current state of each finding.

---

## Table of Contents

1. [The Real Constraint: Prefill Cost](#1-the-real-constraint-prefill-cost)
2. [Concept 1: Context Data Structure & Immutability](#2-concept-1-context-data-structure--immutability)
3. [Concept 2: Compression Gate & Auto-Trigger](#3-concept-2-compression-gate--auto-trigger)
4. [Concept 3: Tree-Shaking (Loop Collapse)](#4-concept-3-tree-shaking-loop-collapse)
5. [Concept 4: LLM-Based Turn Classification](#5-concept-4-llm-based-turn-classification)
6. [Concept 5: Memory & Skill Extraction](#6-concept-5-memory--skill-extraction)
7. [Concept 6: Memory Score & Decay](#7-concept-6-memory-score--decay)
8. [Concept 7: Memory Injection & Prefix Stability](#8-concept-7-memory-injection--prefix-stability)
9. [Concept 8: Budget Enforcement & Headroom](#9-concept-8-budget-enforcement--headroom)
10. [Concept 9: Minimum Context Invariant](#10-concept-9-minimum-context-invariant)
11. [Concept 10: Single-GPU / Same-Session Execution](#11-concept-10-single-gpu--same-session-execution)
12. [Concept 11: Threshold Configurability](#12-concept-11-threshold-configurability)
13. [Concept 12: Real Token Count Feedback](#13-concept-12-real-token-count-feedback)
14. [Concept 13: `apply_compression_memops` Reachability](#14-concept-13-apply_compression_memops-reachability)
15. [Integration: The Missing Links](#15-integration-the-missing-links)
16. [Implementation Plan](#16-implementation-plan)

---

## 1. The Real Constraint: Prefill Cost

Every document in this codebase was written with "KV cache preservation" as the guiding principle. That framing is misleading. Let me correct it.

### What actually costs time

On the user's hardware (RTX 4090, 24GB, llama.cpp, 27B Q4_K_M GGUF, 262K context):

| Operation | Time | Unit cost |
|-----------|------|-----------|
| Full prefill (262K tokens) | 10-30 minutes | ~600-1800 seconds |
| KV extension (append 500 tokens) | ~1-2 seconds | ~1-2 seconds |
| Token generation (1000 tokens) | ~10-30 seconds | ~10-30 ms/token |

The prefill is **600-1800x more expensive** than an equivalent KV extension.

The bottleneck is not "KV cache preservation" — it's **avoiding full prefills**. Two things trigger a full prefill:
1. **The prefix changes** (system prompt modified, messages inserted/removed at the front)
2. **A new HTTP request has a different prefix than the previous request** (server must rebuild KV)

### What the architecture can and cannot do

**CAN do:** Append instructions to the TAIL of the existing conversation as user messages. The KV for the prefix (system prompt + conversation 1..N) is already built. Only the new instruction tokens compute attention.

**CANNOT do:** Avoid the prefill when the compressed context replaces the old one. After compression, the context is completely different — the next turn triggers one prefill of the compressed size. But the compressed context is 2K-10K tokens instead of 262K, so this prefill takes ~1 second instead of 10-30 minutes.

### The correct design principle

> Design for **one unreloadable full prefill per compression cycle**. All compression steps must share the same KV prefix. Never change the system prompt. Never insert or remove messages from the middle of the conversation during compression steps.

---

## 2. Concept 1: Context Data Structure & Immutability

### Current State

File: `include/agent/context.h` (114 lines)

```cpp
class Context {
    std::deque<Message> stack_;     // messages sealed on push
    size_t token_count_ = 0;         // cached estimate (chars/4)

    void push(Message msg) noexcept;
    std::vector<Message> pop(size_t n) noexcept;  // from bottom
    const std::deque<Message>& get_all() const noexcept;
    size_t token_count() const noexcept;
    void clear() noexcept;
};
```

The design is intentionally immutable:
- `push()` seals a message — once on the stack, never modified
- `get_all()` returns a const reference — no mutation
- Token count is a character-based estimate: `(content.size() + reasoning.size()) / 4`

### My Analysis

**What works:**
- The sealed-message contract is enforced. No `.back()` reference can modify pushed messages.
- Token count is cached and incrementally updated on push.

**What's wrong:**

1. **`pop(n)` removes from the BOTTOM, but compression needs atomic replacement.** The compression pipeline needs to take a snapshot, produce compressed output, and replace the stack atomically. `pop()` removes from the bottom one at a time — not atomic.

2. **Token count is a rough estimate.** `message_tokens()` uses `chars / 4`, which is accurate for English prose but off for code (~2-3 chars/token) and JSON (very dense). The gate uses this estimate to decide whether to fire.

3. **There's no `replace()` method.** The clear-then-push pattern has a window where the context is empty. A `replace()` method that atomically swaps the internal deque would be safer.

### Design

**Add `replace()` to Context:**
```cpp
void replace(std::vector<Message> new_msgs) noexcept {
    decltype(stack_)().swap(stack_);  // clear + minimal reallocation
    for (auto& m : new_msgs)
        stack_.push_back(std::move(m));
    recompute_token_count();
}
```

This is O(n) to recompute tokens but eliminates the window between `clear()` and `push()`.

---

## 3. Concept 2: Compression Gate & Auto-Trigger

### Current State

File: `lib/compressor.cpp` lines 19-59

```cpp
class DefaultCompressionGate : public CompressionGate {
    bool should_compress(const Context& context, const Config& agent_cfg) const override {
        if (!threshold_exceeded(context, agent_cfg)) return false;
        if (!sufficient_turns(context)) return false;
        return true;
    }

    bool threshold_exceeded(const Context& context, const Config& agent_cfg) const {
        if (agent_cfg.context_size <= 0) return false;  // ← BUG
        double utilisation = context.token_count() / agent_cfg.context_size;
        return utilisation >= cfg_.threshold;             // default 0.50
    }

    bool sufficient_turns(const Context& context) const {
        return context.size() >= cfg_.min_turns;          // default 10
    }
};
```

### My Analysis

**Three independent failures:**

1. **`context_size = 0` kills the gate.** The auto-detection from `/v1/models` reads `n_ctx` from the server's response. If the server doesn't report it (many local models don't), `context_size` stays 0, and the gate never fires. There's no fallback default.

2. **The compressed result is thrown away.** Even when the gate fires and the pipeline compresses a copy, the result is used for one LLM call and discarded. The live `context_` never shrinks. The next turn sends the full history again.

3. **Cooldown is tracked but never checked.** `set_last_compress_turn()` is called but `is_within_cooldown()` is never consulted in `should_compress()`. The gate could fire every single turn.

### Design

1. **Fix `context_size` fallback**: if `<= 0` after auto-detection and env var, default to `max_tokens * 1.2` (20% overhead).

2. **Add cooldown check to `should_compress()`.**

3. **Persist compressed result to live context**, not just the copy. (Handled in the pipeline integration.)

---

## 4. Concept 3: Tree-Shaking (Loop Collapse)

### Current State

File: `lib/compressor_scanner.cpp` (83 lines)

```cpp
void collapse_loops(std::vector<Message>& history);
```

Scans for consecutive identical tool calls (3+ with same name + arguments). Replaces with a single `[loop collapsed]` note. Called inside `CompressionPipeline::compress()` — which means it only runs if the pipeline runs.

### My Analysis

**What it catches:** Identical tool calls 3+ times in a row. The agent reads the same file with the same arguments.

**What it misses:** Everything that requires semantic understanding — completed investigations, competing branches, irrelevant file reads, dead-end grep results.

**The critical point:** Loop collapse is C++ side (zero LLM calls, zero KV cost). It should always run before any LLM call, regardless of whether the pipeline is invoked.

**The trailing message removal is over-broad.** After finding a loop, it removes ALL subsequent tool/assistant messages — not just the ones that depend on the loop.

### Design

Keep loop collapse as-is. It's a cheap pre-processing step. The semantic tree-shaking happens in Step 1 (classification) — the LLM sees the post-collapse history and decides what's relevant.

Acknowledge the trailing-message over-removal as a known gap but don't prioritize fixing it — loops are rare in practice.

---

## 5. Concept 4: LLM-Based Turn Classification

### Current State

The pipeline sends a single combined request asking for classification, memories, and skills in one JSON response. The prompt is generic — "core = keep, context = archive, prune = drop."

### My Analysis

**The pipeline mechanics are correct for KV extension.** The instruction is appended as a user message. The KV prefix stays valid.

**Three problems with the prompt:**

1. **No task-awareness.** The LLM doesn't know what the current task is. Without this, classification is "keep recent, delete old" — which is what brute-force pop already does.

2. **No work-state model.** The LLM doesn't distinguish completed investigations from active ones. "Prune" without a work-state framework means the LLM guesses.

3. **Combined schema is hard to parse.** One JSON with `classification`, `memories`, `skills` — if one sub-schema is malformed, all are lost.

**The `apply_classification()` function has a UB bug:**

```cpp
for (size_t i = seg.turn_start; i <= end; ++i) {  // turn_start NOT clamped
    tags[i] = seg.tag;
}
```

`turn_end` is clamped but `turn_start` is not. Out-of-range start causes undefined index access.

### Design

**Split into two focused requests:**
- Step 1 (classify): returns a simple JSON array of `{turns, tag, summary}`. Easy to parse, independent error handling.
- Step 2 (extract): returns `{memories: [...], skills: [...]}`. Only runs if step 1 succeeded.

**Redesign the prompt** to use a work-state framework (active investigation, completed investigation, competing branch, dead end).

**Fix the `turn_start` clamp.**

---

## 6. Concept 5: Memory & Skill Extraction

### Current State

`apply_compression_memops()` at `lib/agent.cpp:25-53` is defined in the anonymous namespace but **never called**. It has zero call sites in the entire codebase.

The function applies memory/skill ops to the store, runs `decay_all()`, calls `save()`, and fills the `ExtractionResult` for UI reporting. Every piece of the logic is correct. Nothing ever executes it.

The extraction functions hardcode `evidence_count = 3` instead of using the store's `promote_threshold`.

### Design

1. **Extract `apply_compression_memops()` from the anonymous namespace to a private method on `Agent`.** Name it `Agent::apply_compression_result()`.

2. **Call it from both `compress_now()` and the gate path.**

3. **Use `promote_threshold` instead of hardcoded 3.**

---

## 7. Concept 6: Memory Score & Decay

### Current State

```cpp
void decay_all() override {
    for (auto& [id, mem] : memories_) {
        if (mem.evidence_count > 0)
            mem.evidence_count -= 1;  // always -1, ignores decay_rate
    }
}
```

`ExperienceConfig::decay_rate` (default 0.1) is declared but never read.

### Design

**Proportional decay:**
```cpp
int decay = std::max(1, static_cast<int>(mem.evidence_count * cfg_.decay_rate));
mem.evidence_count -= decay;
```

High-evidence items decay slowly (they're trustworthy). Low-evidence items disappear after a few cycles. The `decay_rate` knob in config is now alive.

---

## 8. Concept 7: Memory Injection & Prefix Stability

### Current State

Every LLM call in `chat_once()` injects memories into the system prompt COPY:

```cpp
if (retriever_) {
    auto suffix = retriever_->build_system_prompt_suffix(user_msg, 500);
    if (!suffix.empty()) {
        for (auto& msg : prompt_copy) {
            if (msg.role == "system") {
                msg.content += suffix;  // MODIFIES SYSTEM PROMPT
                break;
            }
        }
    }
}
```

This changes the system prompt content on every turn, which changes the KV prefix. Every LLM call triggers a full prefill of the (now-modified) system prompt.

### Analysis

**This is the single biggest performance issue in the system.** The memory injection changes the prefix every turn. Every turn pays the full 10-30 minute prefill cost for the system prompt portion. At 262K context, that's catastrophic.

**The compression pipeline is designed correctly** (instructions appended as user messages, prefix unchanged). But the memory injection runs BEFORE the compression pipeline, so even the compression call gets a modified prefix.

### Design

**Memory injection must NOT modify the system prompt.** Instead, inject memories/skills as a SEPARATE message in a fixed slot:

```
[system]          ← NEVER changes
[memory block]    ← updated when store version changes, otherwise untouched
[conversation]    ← grows normally
```

The `[memory block]` is a synthetic message inserted once at session start (or when the store changes). It has its own role (e.g., `system` with a special prefix like `[knowledge]`). The system prompt at index 0 NEVER changes.

**Optimization:** Track a store version counter. Only rebuild the memory block when the store has changed since the last LLM call. If the memory block is the same as last time, the KV prefix for `[system][memory block]` is reused.

---

## 9. Concept 8: Budget Enforcement & Headroom

### Current State

**Does not exist.** No code limits how many tokens the compressed context can occupy. The architecture doc specifies 30% core / 15% archive / 50% headroom but none of these are enforced.

### Design

**25% headroom guarantee:** After compression, the total token count must occupy no more than 75% of the context window. If the LLM over-classifies as "core" and the compressed output violates this, walk backward from the oldest non-system messages and promote them to "context" (replace with archive placeholder).

This is a C++ safety net — it doesn't require an LLM call.

---

## 10. Concept 9: Minimum Context Invariant

### Current State

**Does not exist.** If the LLM classifies everything as "prune", `apply_classification()` returns only the archive message + system prompt. The agent loses all conversation state.

### Design

After classification and before replacing the context, guarantee:

```cpp
// At minimum: system prompt + archive + last user message
bool has_user_message = false;
for (const auto& msg : core)
    if (msg.role == "user") { has_user_message = true; break; }

if (!has_user_message && !history_.empty()) {
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->role == "user") {
            core.push_back(*it);
            break;
        }
    }
}
```

---

## 11. Concept 10: Single-GPU / Same-Session Execution

### Current State

The pipeline uses the same `LLMClient` (same model, same GPU) for both regular turns and compression steps. No second model loading, no VRAM reallocation, no offloading.

### Analysis

**This is already correct** for the compression pipeline. Both Step 1 and Step 2 use `client_` directly.

**The problem is memory injection.** Because it modifies the system prompt, every turn changes the KV prefix. The server must rebuild the KV cache for the system prompt from scratch on every request — even when the conversation prefix hasn't changed.

**The fix is prefix stability** (Concept 7). Keep the system prompt identical, store memories in a separate slot. This ensures the KV prefix is stable across all requests in a session — whether regular turns or compression steps.

---

## 12. Concept 11: Threshold Configurability

### Current State

Config values: `compression_threshold (0.0→0.50), compression_min_turns (0→10), compression_cooldown_turns (0→20)`. All overridable via config file or env vars.

### Analysis

**The knobs are in the right places and the right sizes.** The loading logic (`load_compression_config()`) correctly only overrides defaults when the config value is > 0.

**The issues are:**
1. The threshold is irrelevant when `context_size = 0` (gate never fires)
2. Cooldown is tracked but never checked
3. `min_turns` isn't enforced by `compress_now()`

Fix these, and threshold configurability works as designed.

---

## 13. Concept 12: Real Token Count Feedback

### Current State

`config.h:107` declares `long prompt_tokens_used = -1`. The LLM response's `usage.prompt_tokens` is extracted by `fill_buffered_stats()` and stored in `Stats.prompt_tokens`. But `cfg_.prompt_tokens_used` is **never set**.

### Analysis

**Two layers of dead code:**
1. The config value is never assigned after construction
2. Even if it were assigned, the gate doesn't read it — it uses `context.token_count()` (character estimate)

### Design

After `chat_once()`, write back the real count:
```cpp
if (stats.valid && stats.prompt_tokens > 0)
    cfg_.prompt_tokens_used = stats.prompt_tokens;
```

Update the gate to prefer the real count:
```cpp
double tokens = cfg.prompt_tokens_used > 0
    ? cfg.prompt_tokens_used
    : context.token_count();
```

---

## 14. Concept 13: `apply_compression_memops` Reachability

### Current State

The function at `lib/agent.cpp:25-53` is defined in the anonymous namespace. It has zero call sites. It's structurally perfect (applies ops, decays, saves, fills extraction result) but nothing ever calls it.

### Why It Exists but Never Runs

The function was written as part of the compression pipeline refactor (FIX-004 in the fix tracker). It was intended to bridge compression results to the memory store. But `compress_now()` (the explicit `/compress` command) was never updated to call the pipeline. The gate path throws the compression result away.

### Design

**Extract from anonymous namespace to `Agent::apply_compression_result()`:** Call from both `compress_now()` and the gate path. This single change unlocks the entire memory/skill extraction pipeline.

---

## 15. Integration: The Missing Links

The entire compression system has 12 distinct components, each implemented and individually tested. The system works in pieces but fails as a whole. Two missing links:

**Link 1:** `compress_now()` doesn't call `CompressionPipeline::compress()`. The explicit compression path produces a placeholder.

**Link 2:** `apply_compression_memops()` bridges compression output to memory store input but is never called. The gate path produces `CompressionResponse` but throws it away.

Fix these two links, and 80% of the compression system comes alive.

---

## 16. Implementation Plan

### Phase 0 — Blockers (must fix before anything else works correctly)

| # | Change | Status | Files | Risk | Est. |
|---|--------|--------|-------|------|------|
| 0.1 | Memory injection → separate message slot (never touch system prompt) | ✅ Resolved | `lib/agent.cpp`, `include/agent/agent.h` | 🔴 Fixes prefix stability. Without this, every turn pays 10-30 min prefill. | 4-6h |
| 0.2 | ~~Add `Context::replace()`~~ **Rejected: violates immutable stack.** Compression uses `clear() + push()` instead. Hash-chain integrity (`context.h:43-64`) prevents any future mutation methods. | 🚫 Rejected | `include/agent/context.h` | 🟢 No mutation API — stack architecture enforced at runtime | — |
| 0.3 | Gate checks cooldown in `should_compress()` | ✅ Resolved | `lib/compressor.cpp` | 🟢 Without this, gate fires every turn | 30m |
| 0.4 | Clamp `turn_start` in `apply_classification()` | ✅ Resolved | `lib/compressor_apply.cpp` | 🔴 UB fix — out-of-bounds access | 30m |

### Phase 1 — Links (unlocks the core pipeline)

| # | Change | Status | Files | Risk | Est. |
|---|--------|--------|-------|------|------|
| 1.1 | Extract `apply_compression_memops()` to `Agent::apply_compression_result()` | ✅ Resolved | `lib/agent.cpp`, `include/agent/agent.h` | 🟢 Code already exists, just needs to be reachable | 1h |
| 1.2 | Wire `compress_now()` to pipeline + `apply_compression_result()` | ✅ Resolved | `lib/agent.cpp` | 🟡 Replaces stub with real logic. Pipeline returns original on failure — safe. | 4-6h |

### Phase 2 — Correctness & Safety

| # | Change | Status | Files | Risk | Est. |
|---|--------|--------|-------|------|------|
| 2.1 | Minimum context invariant in `apply_classification()` | ✅ Resolved | `lib/compressor_apply.cpp` | 🟢 Redundant check, prevents corruption | 1h |
| 2.2 | Redesign classification prompt for work-state awareness | ✅ Resolved | `lib/compressor_request.cpp` | 🟡 Prompt change only. Old tests still pass. | 3-4h |
| 2.3 | Split combined request into classify + extract steps | ✅ Resolved | `lib/compressor_request.cpp`, `lib/compressor_parser.cpp`, `lib/compressor.cpp` | 🟡 Architectural change. Both steps share same KV prefix. | 4-6h |
| 2.4 | Gate path persists compressed result to live context | ✅ Resolved | `lib/agent.cpp` | 🟡 Previously throwaway, now persists. Cooldown prevents thrash. | 2-3h |

### Phase 3 — Tuning & Correctness

| # | Change | Status | Files | Risk | Est. |
|---|--------|--------|-------|------|------|
| 3.1 | Proportional decay using `decay_rate` | ✅ Resolved | `lib/memory_store.cpp` | 🟢 One-function change. | 30m |
| 3.2 | Use `promote_threshold` instead of hardcoded 3 | ✅ Resolved | `lib/compressor_apply.cpp` | 🟢 Two-line change. | 30m |
| 3.3 | Feed `prompt_tokens_used` back, update gate to use it | ✅ Resolved | `lib/agent.cpp`, `lib/compressor.cpp` | 🟢 Fixes dead code path. | 1h |
| 3.4 | `context_size` fallback when auto-detection fails | ✅ Resolved | `lib/config.cpp` | 🟢 If server doesn't report n_ctx, use default | 1h |
| 3.5 | Enforce headroom (25% free after compression) | ✅ Resolved | `lib/compressor_apply.cpp` | 🟡 New code path but runs only after successful classification | 3-4h |
| 3.6 | `compress_now()` checks `min_turns` | ✅ Resolved | `lib/agent.cpp` | 🟢 One-line change | 30m |

### Phase 4 — Validation

| # | Change | Status | Files | Risk | Est. |
|---|--------|--------|-------|------|------|
| 4.1 | Integration test for full compression cycle | ✅ Resolved | `tests/run_tests.cpp` (manual) | 🟢 Test-only | 4-6h |
| 4.2 | Archive segment turn range fix | ✅ Resolved | `lib/compressor_apply.cpp` | 🟢 Bugfix in existing code | 1h |

---

### Dependencies

```
Phase 0 (prefix stability, gates, safety) ────── must come first
    │
    ├── Phase 1 (links) ── must have prefix stability
    │   └── Phase 2 (correctness) ── must have working pipeline
    │       └── Phase 3 (tuning) ── optimizations on working system
    │           └── Phase 4 (validation) ── test what's now working
    │
    └── Phase 0.2 (Context::replace) — REJECTED. Stack is pure push/pop/clear.
```

Phase 0.1 (memory injection fix) is the single highest-impact change. It affects every LLM call, not just compression. Without it, every turn pays 10-30 minutes for the system prompt prefill.

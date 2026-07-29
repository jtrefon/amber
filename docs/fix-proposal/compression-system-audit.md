# System Audit: Context Tree-Shaking & Compression

- **Type:** Architecture audit + fix proposal
- **Applies to:** lib/agent.cpp, lib/compressor*.cpp, lib/memory_store.cpp, lib/memory_retriever.cpp, lib/llm.cpp, lib/http_transport.cpp, include/agent/config.h, include/agent/context.h, include/agent/compressor.h, include/agent/experience.h
- **Design patterns:** Strategy, Observer, Memento, Command, Null Object
- **Follows:** AGENTS.md Red → Proposal → Sign-off → Green workflow

---

## 1. Executive Summary

The compression subsystem has three core capabilities that are fully implemented and tested: **loop collapse**, **LLM-based turn classification**, and **memory/skill extraction**. All three are structurally sound. The problem is that **none of them actually run in production** — two of them sit behind a `compress_now()` that's a stub, and the third (automatic gate) throws its result away after one LLM call.

The memory/skill store has a complete scoring and decay engine that never receives input because `apply_compression_memops()` — the function that bridges compression to the store — is **dead code** (defined in the anonymous namespace of `agent.cpp` at line 25, zero call sites).

The compression gate uses a character-based token estimate because `cfg.prompt_tokens_used` is **never set** (declared, documented as "set by Agent after each chat_once", but never assigned anywhere in the codebase). Even if it were set, the gate doesn't read it.

The memory injection path (`MemoryRetriever` in `chat_once()`) invalidates the system prompt's KV cache on every turn by modifying the system message content — which the architecture doc claims is inviolable.

---

## 2. What's Valid in My Previous Proposal

Before I tear it apart, the parts I got right:

| Finding | Correct? | Priority |
|---------|----------|----------|
| `compress_now()` is a stub that bypasses the pipeline | ✅ Correct | 🔴 Critical |
| Automatic gate compresses a copy and discards it | ✅ Correct | 🔴 Critical |
| `MemoryStore` is never fed from compression | ✅ Correct | 🔴 Critical |
| Classification prompt is mechanical, not semantic | ✅ Correct | 🟠 High |
| No budget enforcement | ✅ Correct | 🟠 High |
| `decay_rate` hardcoded to 1 | ✅ Correct | 🟡 Medium |
| Phase ordering (prompt → wiring → gate → budget) | ✅ Correct | — |

Now, what I **missed** or got **wrong**.

---

## 3. What I Missed (The "Small Model" Errors)

### 3.1 🔴 `apply_compression_memops()` is dead code — not just "idle"

I said *"the memory engine is disconnected"* — implying it could be woken up by wiring the pipeline. The truth is worse: the function at `lib/agent.cpp:25-53` in the anonymous namespace is **unreachable**. It has zero call sites:

```
$ grep -rn 'apply_compression_memops' lib/ tui/ tools/ include/
lib/agent.cpp:25: void apply_compression_memops(...)
```

One definition. No callers. The function exists, has the full correct logic (apply memories, apply skills, `decay_all()`, `save()`), but nothing ever passes it a `CompressionResponse`. It's the equivalent of writing a unit test that never runs.

**My proposal treated this as plumbing**. It's not — it's a **functional gap**: an entire subsystem that compiles, passes lint, and does nothing.

### 3.2 🔴 `cfg.prompt_tokens_used` is never set — and the gate doesn't read it anyway

From `config.h:106-107`:
```cpp
// Real token usage from the last LLM call (usage.prompt_tokens), set by
// Agent after each chat_once.
long prompt_tokens_used = -1;
```

The comment says it's set by Agent. **It's not.** A `grep` for assignments produces no results. The Agent collects real token counts via `Stats.prompt_tokens` from `chat()`/`chat_stream()` — and these are surfaced through `AgentHooks::on_stats` for UI display — but **they are never written back to `cfg_.prompt_tokens_used`**.

Even if they were, `DefaultCompressionGate::threshold_exceeded()` doesn't reference it:
```cpp
bool threshold_exceeded(const Context& context, const Config& agent_cfg) const {
    if (agent_cfg.context_size <= 0) return false;
    double utilisation = static_cast<double>(context.token_count()) /
                         static_cast<double>(agent_cfg.context_size);
    return utilisation >= cfg_.threshold;
}
```

The `agent_cfg` is passed but only `context_size` is read. `agent_cfg.prompt_tokens_used` is ignored. The architecture doc promises *"Uses `cfg.prompt_tokens_used / context_size` when real token count is known"* — this is false.

**My proposal mentioned this in the risk table but didn't treat it as a bug.** It's a bug: a documented, declared, dead path.

### 3.3 🔴 Memory injection destroys KV cache every turn — architecture doc is wrong

From `lib/agent.cpp:131-144`:
```cpp
if (retriever_) {
    auto suffix = retriever_->build_system_prompt_suffix(user_msg, 500);
    if (!suffix.empty()) {
        for (auto& msg : prompt_copy) {
            if (msg.role == "system") {
                msg.content += suffix;   // MODIFIES SYSTEM PROMPT
                break;
            }
        }
    }
}
```

Every single LLM call modifies the system prompt copy with injected memories. The architecture doc (`docs/architecture/context-compression.md`) says:

> *"Our approach: append the compression request as a plain user message, keeping the system prompt identical. The KV cache for the system prompt and all prior conversation tokens remains valid."*

**This only works if the system prompt doesn't change.** Every memory injection invalidates that guarantee. Even though it's a copy — the copy is what gets sent to the server. The server rebuilds KV cache for the modified system prompt from scratch on every request.

**Impact:** The "KV cache preservation" claim in the architecture is misleading. The ONLY scenario where it holds is within a single compression call (where the compression request is appended as a user message and the system prompt hasn't been modified before the call). Between turns, every LLM call pays the full context cost.

**My proposal missed this entirely.** It accepted the architecture doc's claim at face value.

### 3.4 🟠 `context_size` auto-detection is fragile — gate never fires without it

`DefaultCompressionGate::threshold_exceeded()` returns `false` immediately if `agent_cfg.context_size <= 0`:
```cpp
if (agent_cfg.context_size <= 0) return false;
```

`context_size` is auto-detected from the server's `/v1/models` response via `n_ctx`. This works for OpenAI API and llama.cpp, but:
- The server must report `n_ctx` explicitly (many local inference backends don't)
- If the probe fails (timeout, network error), `context_size` stays 0
- If the user starts the TUI without a running server, probe fails silently, gate never fires

There's no fallback: no hardcoded default, no configurable maximum, no CLI flag override. The gate simply never triggers.

**This means the entire automatic compression path is disabled by default** for any server that doesn't report context window size. The only way it works is if the user explicitly sets `context_size` in their config file.

### 3.5 🟠 No minimum context invariant after compression

If the LLM classifies everything as "prune" (unlikely but possible with a bad request), `apply_classification()` returns only the archive system message. The agent's context would contain:
```
[0] system: original system prompt
[1] system: "Compressed conversation context: {archive: [], version: 1}"
```

No user messages, no assistant replies, no tool results. The agent would have forgotten what it was doing. `ensure_system_prompt()` would re-push the system prompt, but the conversation state is lost.

The architecture doc says:
> *"If parse succeeds but produces empty history: Keep system prompt + last user message, log"*

This safety net **does not exist**. `apply_classification()` returns whatever survives after pruning, and if nothing survives, it's just the archive message.

**My proposal didn't address this.** It assumed the LLM would always produce reasonable classification.

### 3.6 🟠 `turn_start` not clamped in `apply_classification()`

From `lib/compressor_apply.cpp:23-28`:
```cpp
for (const auto& seg : response.segments) {
    size_t end = std::min(seg.turn_end, history.size() - 1);  // end IS clamped
    for (size_t i = seg.turn_start; i <= end; ++i) {  // start NOT clamped
        tags[i] = seg.tag;
    }
}
```

If the LLM returns `"turns": "999-1000"` for a 50-turn history, `seg.turn_start = 999` is used directly as an array index — **out of bounds**. This is a crash waiting to happen on a bad LLM response. The bug exists at the time of this writing in `compressor_apply.cpp:26`.

### 3.7 🟡 `compress_now()` doesn't enforce `min_turns`

The gate checks `sufficient_turns()`:
```cpp
bool sufficient_turns(const Context& context) const {
    return context.size() >= static_cast<size_t>(cfg_.min_turns);
}
```

But `compress_now()` only checks `context_.size() < 2`. The user can hit `/compress` on a 3-turn conversation and waste an LLM call compressing nothing.

### 3.8 🟡 No integration test for the full cycle

Unit tests exist for:
- `collapse_loops` (3 tests)
- `parse_compression_response` (3 tests)
- `apply_classification` (4 tests)
- `compression_gate` (3 tests)
- `memory_store` (4 tests)
- `MemoryRetriever` (2 tests)

But **no test** wires an Agent with a compressor and memory store, runs turns, triggers compression, and verifies:
- Context is smaller after compression
- Memory store has new entries
- Agent can continue working after compression

The pipeline's LLM call is mocked in unit tests. The actual LLM interaction is only tested manually.

### 3.9 🟡 What the pipeline does vs. what's needed

The current pipeline (in `CompressionPipeline::compress()`) does:
1. `collapse_loops(copy)` — tree-shake identical tool calls
2. `build_compression_request()` — append classification instruction
3. `client.chat(copy, {})` — single LLM call
4. `parse_compression_response(reply)` — extract JSON
5. `apply_classification(copy, cr)` — prune/archive/core

The prompt is the problem. It says "core = keep verbatim, context = archive with summary, prune = drop entirely." It does NOT instruct the LLM to:
- Identify the current active task
- Distinguish completed investigations from active ones
- Collapse completed bug hunts into one-line memory entries
- Prune competing branches and dead ends

The pipeline mechanics are correct. The prompt content is wrong.

---

## 4. The Full Inventory of Issues

| # | Issue | Location | Severity | My Proposal | Reality |
|---|-------|----------|----------|-------------|---------|
| 1 | `compress_now()` doesn't call pipeline | `lib/agent.cpp:215` | 🔴 | ✅ Identified | Correct |
| 2 | Gate discards compressed result | `lib/agent.cpp:150` | 🔴 | ✅ Identified | Correct |
| 3 | `apply_compression_memops()` is dead code | `lib/agent.cpp:25` | 🔴 | ❌ Missed | **No callers** |
| 4 | `prompt_tokens_used` never set, gate ignores it | `lib/config.h:107`, `lib/compressor.cpp:45` | 🔴 | ❌ Listed as risk | **Dead code + dead config** |
| 5 | Memory injection destroys system KV cache | `lib/agent.cpp:131` | 🟠 | ❌ Missed | **Doc claim is false** |
| 6 | Gate never fires when `context_size=0` | `lib/compressor.cpp:47` | 🟠 | ❌ Missed | **Broken auto-path** |
| 7 | No minimum context invariant | `lib/compressor_apply.cpp:83` | 🟠 | ❌ Missed | **Can lose all context** |
| 8 | `turn_start` not clamped in classification | `lib/compressor_apply.cpp:26` | 🟠 | ❌ Missed | **UB on bad LLM response** |
| 9 | `compress_now()` ignores `min_turns` | `lib/agent.cpp:222` | 🟡 | ❌ Missed | **Wasteful on short convos** |
| 10 | No integration test for full cycle | `tests/run_tests.cpp` | 🟡 | ❌ Missed | **Only unit tests** |
| 11 | Shallow classification prompt | `lib/compressor_request.cpp:11` | 🟠 | ✅ Identified | Correct |
| 12 | Budget enforcement missing | `lib/compressor_apply.cpp` | 🟠 | ✅ Identified | Correct |
| 13 | `decay_rate` hardcoded to 1 | `lib/memory_store.cpp:239` | 🟡 | ✅ Identified | Correct |

---

## 5. Root Cause Map

```
compress_now() is a stub (#1)
  |
  ├── Pipeline never runs
  │    ├── collapse_loops never runs → no loop tree-shaking
  │    ├── LLM never classifies → no context pruning
  │    ├── memory ops never extracted → store empty
  │    └── decay_all never called → evidence never decays
  │
  ├── apply_compression_memops is dead (#3)
  │    └── Memory engine is structurally complete but never fed
  │
  └── No minimum context invariant (#7)
       └── Agent can lose all work on bad compression response

Gate path exists but is ineffective
  ├── Compressed copy is thrown away (#2) → context grows unbounded
  ├── Memory injection breaks KV cache (#5) → every turn pays full cost
  ├── context_size=0 blocks gate (#6) → auto-compression disabled by default
  └── prompt_tokens_used never fed back (#4) → gate uses character estimate

Prompt is shallow (#11)
  ├── No task-awareness → LLM classifies by position, not relevance
  └── No summary extraction → completed work is deleted, not archived
```

---

## 6. Target Architecture (Corrected)

### 6.1 Fix priorities

| Priority | Fix | Dependencies | Est. Effort |
|----------|-----|-------------|-------------|
| **P0** | `compress_now()` calls pipeline + `apply_compression_memops()` | None | 4-6h |
| **P0** | Prompt redesign for semantic tree-shaking | None | 2-3h |
| **P1** | Gate persists compressed context | P0 applied | 2-3h |
| **P1** | `apply_compression_memops()` is callable from both gate and `compress_now()` | P0 | Extract |
| **P1** | Minimum context invariant in `apply_classification()` | None | 1h |
| **P1** | Clamp `turn_start` in `apply_classification()` | None | 30min |
| **P2** | Feed `usage.prompt_tokens` back to `cfg_.prompt_tokens_used` | None | 1h |
| **P2** | Fallback `context_size` if auto-detection fails | None | 1h |
| **P2** | Budget enforcement | None | 3-4h |
| **P2** | `compress_now()` checks `min_turns` | None | 30min |
| **P3** | Integration test for full cycle | P0 | 4-6h |
| **P3** | Fix `decay_rate` | None | 30min |

### 6.2 The corrected pipeline flow

```
Agent::compress_now()
  │
  ├─ 1. Snapshot: before = context_.get_all()
  │     tokens_before = context_.token_count()
  │     msgs_before = context_.size()
  │
  ├─ 2. Call pipeline on COPY (immutable — never touch live stack)
  │     auto compressed = compressor_->compress(
  │         std::vector<Message>(before.begin(), before.end()),
  │         cc, client_, &reporter, &cr);
  │
  │     ┌── CompressionPipeline::compress() ──────────────────────┐
  │     │   a. collapse_loops(copy) — tree-shake identical calls  │
  │     │   b. req = build_request() — redesigned semantic prompt │
  │     │   c. copy.push_back(req) — user message, KV preserved   │
  │     │   d. reply = client.chat(copy, {}) — single LLM call    │
  │     │   e. cr = parse_response(reply.content)                  │
  │     │   f. compressed = apply_classification(copy, cr)        │
  │     │   g. enforce_minimum_context(compressed, before)        │
  │     │      // Guarantee: system + archive + last_user_msg     │
  │     │   h. enforce_headroom(compressed, context_size)         │
  │     │      // Guarantee: >= 25% free                           │
  │     │   i. return compressed                                   │
  │     └────────────────────────────────────────────────────────┘
  │
  ├─ 3. Atomic replace
  │     context_.clear();
  │     for (auto& msg : compressed)
  │         context_.push(std::move(msg));
  │
  ├─ 4. Memory engine (extracted to a method, NOT inline)
  │     apply_compression_result(cr, turn_counter_);
  │     // → upsert memories, upsert skills, decay_all, save
  │
  └─ 5. Stats
        r.messages_before = msgs_before
        r.messages_after = context_.size()
        r.tokens_before = tokens_before
        r.tokens_after = context_.token_count()
```

### 6.3 The core insight: one function is the bridge

The single most impactful change is making `apply_compression_memops()` reachable. It already has the correct logic for:
1. Setting `current_turn` on the store
2. Counting upserts vs deprecates
3. Calling `apply_memory_ops()` and `apply_skill_ops()`
4. Running `decay_all()`
5. Calling `save()`
6. Filling `ExtractionResult` for UI reporting

This function needs to be called from both `compress_now()` AND the gate path. It should be extracted from the anonymous namespace to a proper private method on `Agent`.

### 6.4 The prompt: what changes

The current prompt is:
> *"Tag meanings: core = keep verbatim, context = archive, prune = drop"*

The redesigned prompt (from my proposal, still valid) asks the LLM to:
1. **Identify the current active task** from the last N turns
2. **Classify each turn range** as:
   - **core**: active investigation, files currently being modified
   - **prune**: completed investigations (collapsed to one-line summary); competing branches; dead-end file reads; loops
   - **context**: supporting information that may be needed later
3. **Extract memories** for each completed investigation: "Bug X in file Y causing Z was fixed by W in method M"
4. **Extract skills** for reusable patterns discovered

The key difference: the LLM is given a **work-state framework**, not just tag names.

---

## 7. Additional Improvements Beyond the Original Proposal

### 7.1 Minimum context invariant

Before returning from `apply_classification()`, guarantee that at minimum:
1. The original system prompt (index 0)
2. One compressed-context archive message
3. The last user message (so the agent knows what it was doing)

```cpp
// Inside apply_classification(), after building core:
// Guarantee: keep the last user message
bool has_user_message = false;
for (const auto& msg : core)
    if (msg.role == "user") { has_user_message = true; break; }

// No user message means the entire conversation was pruned.
// That's a classification error. Restore the last user message.
if (!has_user_message && !before.empty()) {
    for (auto it = before.rbegin(); it != before.rend(); ++it) {
        if (it->role == "user") {
            core.push_back(*it);
            break;
        }
    }
}
```

### 7.2 Feed real token count from LLM response to the gate

After `chat_once()` completes, extract `usage.prompt_tokens` from the `Stats` and propagate it:

```cpp
// In Agent::chat_once(), after stats are populated:
if (stats.valid && stats.prompt_tokens > 0)
    cfg_.prompt_tokens_used = stats.prompt_tokens;
```

Then update `DefaultCompressionGate::threshold_exceeded()` to prefer the real count:

```cpp
bool threshold_exceeded(const Context& context, const Config& cfg) const {
    if (cfg.context_size <= 0) return false;
    double tokens = cfg.prompt_tokens_used > 0
                    ? static_cast<double>(cfg.prompt_tokens_used)
                    : static_cast<double>(context.token_count());
    double utilisation = tokens / static_cast<double>(cfg.context_size);
    return utilisation >= cfg_.threshold;
}
```

### 7.3 Fallback `context_size` when auto-detection fails

If the server doesn't report `n_ctx`, use a configurable default:

```cpp
// In Config::apply_environment():
if (context_size <= 0) {
    const char* env = std::getenv("AMBER_CONTEXT");
    if (env) {
        context_size = std::atoi(env);
        context_explicit = true;
    }
}
```

And if still 0 after probe + env, use the model's maximum_tokens setting as a heuristic:
```cpp
if (context_size <= 0 && max_tokens > 0)
    context_size = static_cast<int>(max_tokens * 1.2);  // 20% overhead
```

### 7.4 Clamp `turn_start` in `apply_classification()`

One-line fix to `lib/compressor_apply.cpp:26`:
```cpp
size_t start = std::min(seg.turn_start, history.size() - 1);
for (size_t i = start; i <= end; ++i)
```

### 7.5 Add a method `Agent::apply_compression_result()`

Extract `apply_compression_memops()` from the anonymous namespace to a private method on `Agent`:

```cpp
// In agent.h, private section:
void apply_compression_result(const CompressionResponse& cr);

// In agent.cpp:
void Agent::apply_compression_result(const CompressionResponse& cr) {
    if (!memory_store_ || experience_cfg_.store_path.empty()) return;
    if (cr.memory_ops.empty() && cr.skill_ops.empty()) return;

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
```

Then call from both `compress_now()` and the gate path.

---

## 8. Integration Path After Fixes

```
User types "/compress"
  │
  ├─ Agent::compress_now()
  │    ├─ compression_->compress(copy, ...) → LLM classifies
  │    ├─ context_.clear() + push(compressed) → atomic replace
  │    ├─ apply_compression_result(cr) → memory + decay + save
  │    └─ return result
  │
  └─ User sees: "compressed: 40 msgs → 8 msgs, 2 memories extracted"

Automatic gate (next turn)
  │
  ├─ chat_once()
  │    ├─ gate.should_compress() → true (50% utilisation, real tokens)
  │    ├─ compression_->compress(copy, ...)
  │    ├─ context_.clear() + push(compressed) → persists!
  │    ├─ apply_compression_result(cr) → memory + decay + save
  │    ├─ gate.set_last_compress_turn()
  │    ├─ skip memory injection (KV cache preserved)
  │    └─ chat() with compressed history → minimal tokens
  │
  └─ Next turn: cooldown active → no compression → fast
```

---

## 9. Risk Assessment (Revised)

| Risk | Severity | Mitigation |
|------|----------|------------|
| `apply_compression_memops()` accidentally called with null store | Low | Guard at top of method: `if (!store) return;` |
| Gate persists compressed context incorrectly | Medium | Pipeline returns original on LLM failure. Atomic replace: clear + push. If push throws, context is empty — but pipeline only throws on unrecoverable error. Add `try/catch` around replace. |
| Minimum context invariant forces re-evaluation | Low | Only activates when LLM returns all-prune, which is rare with a good prompt |
| `prompt_tokens_used` from one request doesn't match next | Low | It's a heuristic — the estimate is used to decide whether to compress. Being off by 5% doesn't break anything |
| `context_size` fallback is wrong | Medium | Use `max_tokens * 1.2` as heuristic. Better than 0 (gate disabled) |

---

## 10. What to Do Next

The valid parts of my original proposal are the **prompt redesign** (semantic tree-shaking) and the **pipeline wiring**. The parts I missed are the **dead code** (`apply_compression_memops`, `prompt_tokens_used`), the **KV cache conflict**, and the **safety invariants** (minimum context, `turn_start` clamp, `context_size` fallback).

**Recommended order:**
1. Fix `apply_compression_memops()` — extract to `Agent::apply_compression_result()`, call from both paths. This alone unlocks the memory/skill engine
2. Redesign the compression prompt — semantic tree-shaking with work-state awareness
3. Wire `compress_now()` to the pipeline — makes everything run
4. Gate persistence — stop throwing away compressed results
5. Safety invariants — minimum context, headroom, `turn_start` clamp
6. Feed real token counts back to the gate — fix `prompt_tokens_used`
7. Add integration tests — wire agent + memory + compressor + gate
8. Budget enforcement — hard C++ safety net
9. `decay_rate` fix — minor cleanup

---

## 11. References

| Document | Content | Status |
|----------|---------|--------|
| `docs/architecture/context-compression.md` | Original design doc — KV cache claim is incorrect | Needs update |
| `docs/spec/compression/compression-pipeline.md` | Pipeline contract — structurally correct | Valid |
| `docs/spec/compression/turn-classification.md` | Classification model — `turn_start` bug present | Needs fix |
| `docs/spec/compression/loop-collapse.md` | Loop collapser — trailing message removal is brittle | Known gap |
| `docs/spec/memory/memory-store.md` | Memory store — `decay_rate` ignored | Known gap |
| `docs/spec/memory/extraction.md` | Extraction pipeline — `apply_compression_memops` references are stale | References dead code |
| `docs/issues.md` | Issue register — all resolved | Does not include these issues |
| `docs/fix-tracker.md` | Prior refactors — does not include compression pipeline | Stale |
| `AGENTS.md` | Engineering principles | Valid |

---

## 12. Sign-off

| Role | Name | Date | Status |
|------|------|------|--------|
| Author | Frank | 2026-07-29 | ✍️ |
| Reviewer | | | ⬜ |
| Sign-off | | | ⬜ |

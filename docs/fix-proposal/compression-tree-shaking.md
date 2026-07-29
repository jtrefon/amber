# Fix Proposal: Context Tree-Shaking & Compression Pipeline

- **Status:** Proposal — awaiting sign-off
- **Applies to:** `lib/agent.cpp`, `lib/compressor_request.cpp`, `lib/compressor_apply.cpp`, `lib/memory_store.cpp`, `lib/compressor.cpp`
- **Design patterns:** Strategy, Observer, Memento, Command, Null Object

---

## 1. Problem Statement

The agent loop produces two kinds of context waste. The pipeline we have only addresses one.

### 1.1 Token waste

Every conversation accumulates dead weight:
- **Verbose file reads** — the agent reads 200 lines of `foo.cpp` to find one function
- **Dead-end investigations** — the agent greps for X, finds nothing, moves on. The grep + its results stay in context forever
- **Resolved bugs** — bug A was found and fixed 30 turns ago, but every turn since pays the tax of those file reads, grep results, and tool outputs
- **Competing branches** — the agent explored approach A (5 turns), rejected it, then explored approach B (current). The approach A detour is still in context

### 1.2 KV cache invalidation

Every time the system prompt changes, the entire KV cache is invalidated. The model recomputes every prefix token from scratch. This is the dominant latency cost for any agent that:
- Injects memories/skills into the system prompt per turn
- Changes agent mode mid-conversation
- Uses a dynamic system prompt

### 1.3 Why the current pipeline doesn't solve this

The automatic gate in `chat_once()` *does* call the compression pipeline, but it compresses a **copy** of the prompt, uses it for one LLM call, then discards it. The live context grows unbounded. The pipeline never persists.

The manual `/compress` command (`compress_now()`) **bypasses the pipeline entirely** — it brute-force pops messages from the bottom and pushes a placeholder `"[compressed: N messages removed...]"`.

Neither path:
- Persists the compressed result to the live context
- Feeds memories/skills back into the store
- Runs the decay/scoring engine
- Leaves guaranteed headroom for the next LLM call

---

## 2. Current State

### 2.1 `compress_now()` — the stub

```cpp
// lib/agent.cpp:215-254
CompressionResult Agent::compress_now() {
    size_t keep = cfg_.compression_min_turns > 0
                  ? cfg_.compression_min_turns : 10;
    size_t pop_count = context_.size() - keep - 1;
    context_.pop(pop_count);                              // brute-force delete
    summary_msg.content = "[compressed: "                 // placeholder text
        + std::to_string(pop_count) + " earlier messages removed...]";
    context_.push(std::move(summary_msg));
    return r;
}
```

No LLM call. No classification. No memory. No decay. Just POP + TEXT.

### 2.2 The gate in `chat_once()` — the working but wasted path

```cpp
// lib/agent.cpp:147-154
if (gate_->should_compress(context_, cfg_)) {            // gate fires
    auto compressed = compression_->compress(prompt_copy, cc, client_);
    prompt_copy.assign(compressed.begin(), compressed.end());  // uses for this call only
    gate_->set_last_compress_turn(turn_counter_);
}
```

The pipeline runs. Classification happens. But the result is **thrown away** after the LLM call. The live `context_` never shrinks. Next turn: full context again, gate fires again, pipeline runs again.

### 2.3 The compression request prompt — mechanical, not semantic

```cpp
// lib/compressor_request.cpp:11-45
req.content = R"(...
Tag meanings:
  "core"    = keep verbatim — active task, recent turns, decisions, preferences
  "context" = archive with summary — useful context but not immediately needed
  "prune"   = drop entirely — stale tool output, superseded attempts, loops
...
Guidelines:
  - "name" should be short, unique, kebab-case, and descriptive
  - Keep content under 200 tokens per entry
  - Use contiguous turn ranges for classification)";
```

This tells the LLM WHAT the tags mean, but not HOW to decide. There is no:
- Guidance on identifying the current active task
- Heuristics for completed vs. active investigations
- Rules for when a file read is "done" vs "still needed"
- Instructions for collapsing completed work into summary lines

The LLM is asked to classify, but given no semantic framework to do it well.

### 2.4 The loop collapser — catches identical tool calls only

```cpp
// lib/compressor_scanner.cpp:12-81
// Detects: same tool name + same arguments, 3+ times
// Replaces with: "[loop collapsed] turns X-Y: tool loop detected, N calls collapsed"
```

This catches the obvious case (script keeps reading the same file). But it doesn't catch:
- Different files, same class of investigation (bug A → bug B → bug C)
- Completed investigation chains (read → grep → test → fix → done)
- Competing branches (tried approach A, failed, moved to B)

---

## 3. Root Cause Analysis

### RC1: Tree-shaking has no semantic model

The system has no concept of **work state**. It doesn't know what task is current, what's been completed, what's abandoned. Without this model, compression is just "delete old stuff and hope for the best."

The classification prompt treats all turns as equal — it doesn't instruct the LLM to distinguish between:
- **Completed work** (investigation reached a conclusion, fix was applied → collapse to summary)
- **Active work** (current investigation, files being modified → keep verbatim)
- **Dead ends** (explored and rejected → prune entirely)
- **Supporting context** (file headers, configs, test results → may be archived)

### RC2: Pipeline output never reaches the live context

The gate compresses a copy → uses it → discards it. The live `context_` grows until `compress_now()` brute-force pops. This means:
- Every turn after the gate fires still sends the full history to the LLM
- The compression cost is paid every turn (re-computing what was already computed)
- The 25% headroom you designed for is never enforced

### RC3: Memory/skills/decay are disconnected

The pipeline *can* extract memories and skills from the classification response. The `CompressionResponse` struct has `memory_ops` and `skill_ops` fields. The `apply_memory_ops()` / `apply_skill_ops()` functions exist. But `compress_now()` never calls them, and the gate only operates on a throwaway copy.

The decay engine (`MemoryStore::decay_all()`) is wired in `apply_compression_memops()` in `agent.cpp` but only fires when compression produces non-empty ops — which is never, because compression never runs.

### RC4: The context structure isn't stack-friendly for the workload

The `Context` class (`include/agent/context.h`) is an immutable deque:
- `push(msg)` — appends to top
- `pop(n)` — removes from bottom
- `get_all()` — read-only view

This is fine for incremental growth. But compression needs **atomic replacement**:
1. Snapshot the full stack
2. Call pipeline (operates on copy)
3. Clear the stack
4. Push the compressed result

The current `compress_now()` does step 3 via `pop()` (removes from bottom, one message at a time, while `size()` changes mid-operation) — which is fragile, not atomic, and leaves the stack in an inconsistent state if interrupted.

### RC5: `decay_rate` is hardcoded to 1

`MemoryStore::decay_all()` always decrements `evidence_count` by exactly 1, ignoring the configured `decay_rate`. This means items decay at a fixed rate regardless of configuration, and the tuning knob is dead code.

---

## 4. Target Architecture

### 4.1 Core insight: tree-shaking is the product, compression is the mechanism

The pipeline exists to enable semantic tree-shaking. The wiring, memory integration, and budget enforcement are supporting actors. The heart of the fix is redesigning **what the LLM is asked to do during compression**.

### 4.2 The semantic model

Every conversation has a **current task** identified from the most recent user messages and tool calls. Every turn is classified relative to that task:

| Work State | Classification | Action | Token Budget |
|------------|---------------|--------|-------------|
| Current task (active investigation, file being modified) | **core** | Keep verbatim | 30% |
| Supporting context (file headers, configs, test runs) | **context** | Archive with summary | 15% |
| Completed investigation (found bug, applied fix) | **prune** | Remove entire chain | — |
| Dead end (explored, rejected, moved on) | **prune** | Remove entirely | — |
| Loop (identical tool calls 3+ times) | **prune** | Collapse before classification | — |
| Irrelevant tool output (file read outside current scope) | **prune** | Remove | — |

The crucial distinction is **completed investigation** vs. **active investigation**:
- **Completed**: the agent found the root cause, applied a fix, verified it passes. The entire investigation chain (file reads, grep searches, false starts) is **irrelevant** now. Replace with: *"Bug X in file Y.causing Z was identified and fixed by W"*
- **Active**: the agent is still reading files, searching for causes. This is **core** — keep verbatim so the agent can continue

### 4.3 Pipeline flow (revised for single-GPU compliance)

```
┌──────────────────────────────────────────────────────────────────────┐
│                     compress_now() / gate path                        │
│                                                                      │
│  1. Snapshot: before = context_.get_all()                            │
│     (immutable — never mutate the live stack during pipeline)         │
│                                                                      │
│  2. Reserve headroom: target_tokens = context_size * 0.75            │
│     (leave 25% for next LLM response)                                │
│                                                                      │
│  3. Pipeline: compressed = compressor_->compress(before, cc,         │
│                                                  client_, &cr)       │
│     ┌────────────────────────────────────────────────────────┐      │
│     │ Inside CompressionPipeline::compress():                 │      │
│     │   a. copy = history  (operate on copy throughout)       │      │
│     │   b. collapse_loops(copy)   — tree-shake identical      │      │
│     │   c. req = build_request()  — REDESIGNED PROMPT         │      │
│     │   d. copy.push_back(req)    — append as user msg        │      │
│     │   e. reply = client.chat(copy, {})  — single LLM call  │      │
│     │   f. cr = parse_response(reply.content)                 │      │
│     │   g. copy = apply_classification(copy, cr)             │      │
│     │   h. copy = enforce_budget(copy, target_tokens)        │      │
│     │   i. return copy                                       │      │
│     └────────────────────────────────────────────────────────┘      │
│                                                                      │
│  4. Atomic replace: context_.clear();                               │
│     for (auto& msg : compressed) context_.push(std::move(msg))       │
│     (if pipeline fails, compressed == before — no data loss)         │
│                                                                      │
│  5. Memory engine:                                                   │
│     memory_store_->apply_memory_ops(cr.memory_ops)                   │
│     memory_store_->apply_skill_ops(cr.skill_ops)                     │
│     memory_store_->decay_all()                                       │
│     memory_store_->save(path)                                        │
│                                                                      │
│  6. Stats: r.messages_before, r.messages_after,                     │
│            r.tokens_before, r.tokens_after = context_.token_count()  │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.4 Key architectural properties

1. **Single LLM call** — classification and extraction happen in one request. No second model, no second context load.
2. **Same system prompt** — the compression request is appended as a user message. The system prompt at `history_[0]` never changes. KV cache for all prior tokens is preserved.
3. **Atomic context replace** — `clear()` then `push()`. If the pipeline fails midway (LLM timeout, parse error), the returned history is the original input. No corruption, no inconsistency.
4. **25% headroom** — `enforce_budget()` guarantees that after compression, at least 25% of the context window is vacant for the next LLM response.
5. **Memory/skill persistence** — every compression extracts knowledge from completed work and feeds it into the store. The decay engine runs every cycle.

---

## 5. Tree-Shaking: The Prompt Redesign

The compression request prompt is the single most important piece. It must teach the LLM to do semantic tree-shaking.

### 5.1 Current prompt (broken)

```text
Tag meanings:
  "core"    = keep verbatim — active task, recent turns, decisions, preferences
  "context" = archive with summary — useful context but not immediately needed
  "prune"   = drop entirely — stale tool output, superseded attempts, loops
```

The LLM doesn't know what "active task" means. It doesn't know what "stale tool output" looks like versus "important tool output."

### 5.2 Redesigned prompt

The redesign changes three things:
1. **Asks for work-state classification** instead of generic tags
2. **Defines heuristics** for each state
3. **Requires summaries for completed work** that collapse entire investigations to one sentence

```text
Analyze the conversation above. Your job is to reduce token waste while
preserving everything needed to continue the current task.

First, identify the CURRENT ACTIVE TASK from the last user messages and
recent tool calls. What is the agent actively working on right now?

Then classify EVERY turn range into one of three categories:

--- "core" (keep verbatim) ---
Keep verbatim ONLY what is needed to continue the current task:
- The current active investigation
- Files currently being modified (reads + writes + results for active bug)
- Decisions and preferences that affect the current work
- TODO lists and pending actions

Prune everything else from the investigation chain.

--- "prune" (remove entirely) ---
Remove everything that is no longer needed:

1. COMPLETED INVESTIGATIONS:
   Bug that was found and fixed? Remove the entire investigation:
   file reads, grep results, test output, fix code. Replace with
   a summary memory entry (see "memories" below).

2. COMPETING BRANCHES:
   The agent tried approach A for 5 turns, rejected it, moved to B?
   Remove approach A entirely. It's a dead end.

3. COMPLETED FILE READS:
   The result of reading a file was used once, and the information
   was acted on (fix applied, config changed)? The file content is
   no longer needed. Remove it.

4. IRRELEVANT TOOL OUTPUT:
   Grep for X that found nothing? File read that showed code not
   related to the current task? Remove it.

5. LOOPS AND RETRIES:
   Tool calls that failed and were retried? Identical file reads?
   Remove all but the last attempt.

--- "context" (archive with summary) ---
For turns that are NOT part of the current task but provide useful
background:
- Commands or workflows the agent discovered (e.g. "tests use make test")
- Configuration details that may be needed later
- Decisions that are settled but could be revisited

Replace these with a one-line summary in the archive.

MEMORIES: For each COMPLETED investigation, create ONE memory entry:
{
  "name": "bug-fix-segfault-in-parse",
  "content": "Bug X in file Y function Z causing segfault on empty input. Fixed by adding null check at line 42.",
  "tags": ["bug", "parsing", "segfault"],
  "action": "upsert"
}

SKILLS: For each reusable pattern discovered, create ONE skill entry:
{
  "name": "running-tests",
  "content": "Tests are run via 'make test'. The run_tests binary has 150+ tests.",
  "trigger_phrase": "test",
  "action": "upsert"
}
```

### 5.3 Why this works

The LLM sees the full conversation. It can trivially identify:
- Which tool calls are active (last 3-5 turns) vs. historical (20+ turns ago)
- Which file reads were superseded (read file → applied fix → on to next task)
- Which investigation chains are complete (start with a user request, end with a fix confirmation)
- Which branches were competing (tried A first, then B, now on B)

The prompt gives the LLM the **semantic framework** to make these decisions, not just tag names.

### 5.4 Examples

**Scenario 1: Bug hunt — 5 bugs fixed, 1 in progress**

Conversation: ~55 turns, 5 bugs found and fixed, currently investigating bug 6

Before:
```
[0] user: find all bugs in the parser
[1-4] assistant: reads parser.cpp, grep for patterns → fixes bug A
[5-8] assistant: reads lexer.cpp, grep for patterns → fixes bug B
[9-12] assistant: reads ast.cpp, grep for patterns → fixes bug C
[13-16] assistant: reads codegen.cpp, grep for patterns → fixes bug D
[17-20] assistant: reads optimizer.cpp, grep for patterns → fixes bug E
[21-24] assistant: reads typecheck.cpp, grep for patterns → CURRENT (bug F)
[25-26] assistant: writes fix, runs tests, done
```

After (with redesigned prompt):
```
[core]   [21-24] assistant: reads typecheck.cpp, grep for patterns → CURRENT (bug F)
[core]   [25-26] assistant: writes fix, runs tests, done

pruned: [0-20] — bugs A-E were investigated and fixed

memories: {
  "name": "bug-fix-null-ptr-in-parser",
  "content": "Bug in parser.cpp at line 42: null pointer dereference on empty input. Fixed by adding null check before strcmp call.",
  "tags": ["bug", "parser", "null-ptr"]
}
memories: {
  "name": "bug-fix-infinite-loop-in-lexer",
  "content": "Bug in lexer.cpp at line 88: infinite loop on EOF without newline. Fixed by adding eof check in while condition.",
  "tags": ["bug", "lexer", "infinite-loop"]
}
```

Tokens saved: ~80%. All completed work is replaced by memory entries. The agent continues with full context for bug F.

**Scenario 2: New feature — 3 files read, 1 being modified**

Before:
```
[0] user: add a --verbose flag to the CLI
[1] assistant: reads main.cpp (200 lines)
[2] assistant: reads cli.h (80 lines)
[3] assistant: reads cli.cpp (150 lines) — this is the file to modify
[4] assistant: reads argparser.h (60 lines) — dead end, wrong file
[5] assistant: back to cli.cpp, finds option parsing code
[6+] assistant: implements the flag
```

After:
```
[core]   [3] assistant: reads cli.cpp (150 lines) — current file
[core]   [5] assistant: finds option parsing code in cli.cpp
[core]   [6+] assistant: implements the flag

pruned: [1-2] — main.cpp and cli.h read to understand context, no longer needed
pruned: [4] — argparser.h was a dead end, not needed

memories: {
  "name": "cli-option-location",
  "content": "CLI option parsing lives in cli.cpp around line 120. Options use the getopt_long pattern with a static options array.",
  "tags": ["cli", "options", "getopt"]
}
```

Tokens saved: ~35%. The relevant file read is kept; the context reads and dead end are pruned.

**Scenario 3: Multiple competing approaches**

Before:
```
[0] user: refactor the config loading to support YAML
[1] assistant: approach A — reads libconfig source, writes proto
[2-5] assistant: fails — libconfig doesn't support YAML natively
[6] assistant: switches to approach B — yaml-cpp
[7-8] assistant: reads yaml-cpp docs, writes new ConfigYaml class
[9+] assistant: integrates yaml-cpp, removes libconfig dependency
```

After:
```
[core]   [6] assistant: switches to approach B — yaml-cpp
[core]   [7-8] assistant: reads yaml-cpp docs, writes new ConfigYaml class
[core]   [9+] assistant: integrates yaml-cpp

pruned: [1-5] — approach A failed, no longer relevant

memories: {
  "name": "yaml-config-lib",
  "content": "Config loading refactored from libconfig to yaml-cpp. New class ConfigYaml in config_yaml.cpp. libconfig dependency removed.",
  "tags": ["config", "yaml", "refactor"]
}
```

Tokens saved: ~50%. Competing branch is pruned entirely with no loss of context for the current work.

---

## 6. Implementation Plan

### Phase 1: Compression request prompt redesign (the tree-shaking engine)

**Files:** `lib/compressor_request.cpp`

Replace the mechanical classification prompt with the semantic tree-shaking prompt from section 5.2.

**Verification:**
- [ ] Prompt asks the LLM to identify the current active task
- [ ] Prompt defines clear heuristics for completed vs. active investigations
- [ ] Prompt requires one-line summaries for pruned work
- [ ] Prompt structures memory/skill entries from completed work
- [ ] `make test` passes (prompt content is tested via `request_builder_returns_message`)

### Phase 2: Wire `compress_now()` to the pipeline

**Files:** `lib/agent.cpp`

```cpp
CompressionResult Agent::compress_now() {
    if (!compression_ || context_.size() < 2) return {};

    // Snapshot — immutable
    auto before = context_.get_all();
    size_t msgs_before = before.size();
    size_t tokens_before = context_.token_count();

    CompressionResult r;
    CompressionReporter reporter(hooks_, r);
    reporter.set_before(msgs_before, tokens_before);

    // Call pipeline (operates on copy)
    auto cc = load_compression_config(cfg_);
    CompressionResponse cr;
    auto compressed = compression_->compress(
        std::vector<Message>(before.begin(), before.end()),
        cc, client_, &reporter, &cr);

    // Atomic replace
    context_.clear();
    for (auto& msg : compressed)
        context_.push(std::move(msg));

    // Memory engine
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
- [ ] `/compress` with 20+ turns: LLM classification runs, not brute-force pop
- [ ] `/compress` after tool loops: `collapse_loops` runs first
- [ ] Memory store file updated with memories/skills from LLM response

### Phase 3: Persist automatic gate compression to live context

**Files:** `lib/agent.cpp`

```cpp
// In chat_once(), after pipeline returns compressed result:
if (gate_->should_compress(context_, cfg_)) {
    auto cc = load_compression_config(cfg_);
    CompressionResponse cr;
    auto before = context_.get_all();
    auto copy = std::vector<Message>(before.begin(), before.end());
    auto compressed = compression_->compress(copy, cc, client_, nullptr, &cr);

    // Persist to live context so every subsequent turn uses the compressed version
    context_.clear();
    for (auto& msg : compressed)
        context_.push(std::move(msg));

    // Memory engine (same as compress_now)
    if (memory_store_ && !cr.memory_ops.empty()) { ... }

    gate_->set_last_compress_turn(turn_counter_);
}
```

**Verification:**
- [ ] Gate fires → `context_.size()` decreases
- [ ] Cooldown prevents re-compression for N turns
- [ ] System prompt at `context_[0]` never modified

### Phase 4: Fix `decay_rate` in MemoryStore

**Files:** `lib/memory_store.cpp`

```cpp
void decay_all() override {
    for (auto& [id, mem] : memories_) {
        mem.evidence_count = std::max(0,
            static_cast<int>(mem.evidence_count * (1.0 - cfg_.decay_rate)));
        if (mem.evidence_count <= 0) mem.promoted = false;
    }
    for (auto& [id, sk] : skills_) {
        sk.evidence_count = std::max(0,
            static_cast<int>(sk.evidence_count * (1.0 - cfg_.decay_rate)));
        if (sk.evidence_count <= 0) sk.promoted = false;
    }
}
```

**Verification:**
- [ ] `decay_rate = 0.1` reduces evidence by 10% (not 1)
- [ ] Items at 0 evidence set to `promoted = false`

### Phase 5: Budget enforcement after classification

**Files:** `lib/compressor_apply.cpp`

After `apply_classification()` returns the compressed history, enforce the 25% headroom guarantee:

```cpp
std::vector<Message> enforce_headroom(
    const std::vector<Message>& compressed,
    size_t context_size_tokens) {

    // Count tokens in compressed result
    size_t used = 0;
    for (const auto& msg : compressed)
        used += message_tokens(msg);

    // Target: at least 25% of context window free
    size_t target = context_size_tokens * 0.75;
    if (used <= target) return compressed;

    // Over budget: promote oldest non-system turns from core → context
    auto result = compressed;
    for (size_t i = result.size() - 1; i > 0; --i) {
        if (result[i].role != "system") {
            result[i].content = "[over-budget archived]";
            used -= message_tokens(compressed[i]);
            used += message_tokens(result[i]);
            if (used <= target) break;
        }
    }
    return result;
}
```

Called inside `CompressionPipeline::compress()` after `apply_classification()`.

**Verification:**
- [ ] After compression, `context_.token_count() <= context_size * 0.75`
- [ ] System prompt and archive blocks are never modified
- [ ] Test with mocked LLM returning all-core classification over budget

---

## 7. What Each Phase Fixes

| Issue | Phase | Before | After |
|-------|-------|--------|-------|
| No semantic tree-shaking | 1 | "core = keep, prune = drop" | Work-state-aware classification |
| compress_now bypasses pipeline | 2 | brute-force pop + placeholder | Full LLM classification |
| Memory engine disconnected | 2 | store never updated | upsert + decay + save |
| Context mutation | 2 | context_.pop() mid-loop | Atomic clear + push |
| Gate compresses but discards | 3 | copy used once, thrown away | Persisted to live context |
| KV cache thrashing | 3 | context grows forever | Compressed context persists |
| decay_rate ignored | 4 | always decrements by 1 | Uses configured rate |
| No headroom guarantee | 5 | overflows on next LLM call | 25% always free |

---

## 8. Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| LLM doesn't follow new prompt | Medium | Fallback: if parse fails, pipeline returns original history unchanged |
| Extra LLM call on /compress | Low | Single call, same context. Gate already does this |
| Gate now mutates live context | Medium | Cooldown prevents thrashing. Pipeline returns original on failure |
| Classification removes needed context | Medium | Budget enforcement is C++ safety net. Archived context is still in JSON block |
| prompt_tokens_used not available | Medium | Gate falls back to character-based estimate when real tokens are unknown |

---

## 9. Dependencies and Ordering

```
Phase 1 (prompt redesign) ─── tree-shaking logic ───────────────────────┐
    Phase 2 (wire pipeline) ── makes the engine actually run ──────────┤
        Phase 3 (gate persistence) ── no more throwaway compression ──┤
            Phase 4 (decay_rate fix) ─────────────────────────────────┤
                Phase 5 (headroom enforcement) ───────────────────────┘
```

Phases 1-2 are the critical path. Phase 1 changes only the prompt (no code wiring). Phase 2 does the heavy lifting. 3-5 are incremental safeguards.

---

## 10. Verification Checklist

### Build & test
- [ ] `make clean && make && make test` passes
- [ ] `make lint` clean (no new clang-tidy warnings)
- [ ] `make analyze` clean (no new cppcheck warnings)

### Functional
- [ ] `/compress` with 20+ turns runs LLM classification (not brute-force pop)
- [ ] Completed investigations are pruned, summary memory entries created
- [ ] Active investigation turns are kept verbatim
- [ ] Competing branches are pruned
- [ ] Memory store file updated after compression
- [ ] `decay_rate` config respected
- [ ] Context immutability preserved (pipeline operates on copy)
- [ ] System prompt at `context_[0]` never modified

### Integration
- [ ] Automatic gate persists compressed context to live stack
- [ ] Two consecutive LLM calls after gate use the same compressed context
- [ ] Cooldown prevents re-compression for N turns
- [ ] After compression, at least 25% of context window is free

### Manual scenario tests
- [ ] Bug hunting: 5 fixed + 1 in progress → 90% tokens reclaimed
- [ ] New feature: file reads pruned, only active modification kept
- [ ] Competing approaches: rejected branch pruned entirely
- [ ] Dead-end grep: pruned, not in context

---

## 11. References

| Document | Relationship |
|----------|-------------|
| `docs/architecture/context-compression.md` | Design authority — this proposal implements its vision |
| `docs/spec/compression/compression-pipeline.md` | Pipeline contract — phase 2 wires to this |
| `docs/spec/compression/turn-classification.md` | Classification model — phase 1 redesigns this |
| `docs/spec/compression/loop-collapse.md` | Loop collapser — unchanged, phase 1 still uses it |
| `docs/spec/memory/memory-store.md` | Memory/skill store — phase 2 integrates it |
| `docs/spec/memory/extraction.md` | Extraction pipeline — phase 2 feeds it from compression |
| `docs/issues.md` | Issue register — RC1-4 in this doc map to issues there |
| `docs/fix-tracker.md` | Prior refactors — this replaces FIX-004 |
| `AGENTS.md` | Engineering principles — this follows Red → Proposal → Sign-off → Green |

---

## 12. Sign-off

| Role | Name | Date | Status |
|------|------|------|--------|
| Author | Frank | 2026-07-29 | ✍️ |
| Reviewer | | | ⬜ |
| Sign-off | | | ⬜ |

**Next step:** Reviewer approves → implement Phase 1 (prompt) + Phase 2 (wiring) → test → merge.

# Context Compression Architecture

- **Status:** Design proposal
- **Applies to:** `lib/` (core domain), `include/agent/` (ports)
- **Depends on:** `LLMClient`, `Message`/`history_`, `Config`, `json`
- **Design patterns:** Strategy, Composite, Factory, Value Object

---

## Problem

The agent loop appends every turn — user messages, assistant replies, tool calls,
tool results, thinking blocks — to an ever-growing `history_`. Every iteration
sends the **entire** history to the LLM. As the conversation grows:

- **Cost** increases linearly with total token count.
- **Latency** increases (longer prefill per LLM call).
- **Quality** degrades at scale: "lost in the middle" effect.
- **Hard limits** are reached when the context window overflows.

A 128K-token context window fills up in 300–500 turns of a typical agent loop.
Tool-heavy conversations consume tokens even faster.

---

## Core Insight: KV Cache Is the Bottleneck

Every LLM call processes the full prompt through the attention layers. Transformer
KV caches store the key/value projections for every prefix token. When the system
prompt changes, **the entire KV cache is invalidated** — the model recomputes
every token from scratch on the next call.

Multi-phase compression (classify, then summarize, then extract memories, each
with a different system prompt) causes N full recomputations. This is why other
agents (hermess) take 8+ minutes — they pay the full context cost per phase.

**Our approach: append the compression request as a plain user message, keeping
the system prompt identical.** The KV cache for the system prompt and all prior
conversation tokens remains valid. The model only computes attention for the
request tokens and its generated response. Marginal cost: O(response_tokens).

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    C++ Layer (lib/)                              │
│                                                                  │
│  ┌──────────────┐   ┌──────────────┐   ┌─────────────────────┐  │
│  │ Loop Scanner │──▶│ Collapser    │──▶│ Prompt Builder       │  │
│  │ (detect dup) │   │ (dedupe)     │   │ (append request msg) │  │
│  └──────────────┘   └──────────────┘   └──────────┬──────────┘  │
│                                                    │              │
│                                                    ▼              │
│                                          ┌──────────────────┐    │
│                                          │ LLM call          │    │
│                                          │ (same system,     │    │
│                                          │  user msg request)│    │
│                                          └────────┬─────────┘    │
│                                                   │              │
│             ┌─────────────────────────────────────┼──┐           │
│             ▼            ▼            ▼           ▼  ▼           │
│        ┌────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐        │
│        │Prune   │ │Upsert    │ │Deprecate │ │Archive   │        │
│        │history │ │memories  │ │stale     │ │summary   │        │
│        └────────┘ └──────────┘ └──────────┘ └──────────┘        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Key property: one LLM call, zero system prompt changes

The entire compression pipeline runs as a single request-response cycle with
no system prompt swap. KV cache stays valid from before the request through
the entire response generation.

---

## Pre-processing: Loop Infection Prevention

Before the compression request touches the LLM, the C++ layer scans `history_`
for detected loops and collapses them. This prevents the model from seeing
its own failure patterns and compounding them.

### Loop detection (already exists in `Agent::run()`)

- **Tool loop**: same tool name + arguments repeated ≥3 times
- **Text loop**: same content repeated ≥5 times

### Collapse rules

```
Before collapse:
  user: fix the bug
  assistant: [reads file] → tool: file content → assistant: [reads file] → tool: file content → assistant: [reads file] → tool: file content
  assistant: done.

After collapse:
  user: fix the bug
  context note: "turns 1-6: tool read was repeated 3 times, result identical each time - collapsed"
  assistant: done.
```

Collapsed entries become a `context`-tagged `ArchiveEntry` in the compressed
output. No information is lost — the model sees that a loop happened and was
handled.

---

## The Compression Request

The C++ layer appends one user message to `history_`:

```
<compression>
Analyze the conversation above and produce a JSON response with the following exact structure.
Do not include any text outside the JSON block.

{
  "classification": [
    {
      "turns": "0-2",
      "tag": "core",
      "summary": ""
    },
    {
      "turns": "3-7",
      "tag": "context",
      "summary": "investigated file structure using grep and read tools, found config location"
    },
    {
      "turns": "8-12",
      "tag": "prune",
      "summary": ""
    }
  ],
  "memories": [
    {
      "content": "Build output lands in repo root, not build/",
      "tags": ["build", "makefile"],
      "action": "upsert"
    }
  ],
  "skills": [
    {
      "content": "To run tests: make test, expects 87 passed",
      "trigger_phrase": "test",
      "action": "upsert"
    },
    {
      "content": "Old broken workflow using cmake --build",
      "trigger_phrase": "build",
      "action": "deprecate"
    }
  ]
}

Tag meanings:
  "core"    = keep verbatim — active task, recent turns, decisions, preferences
  "context" = archive with summary — useful context but not immediately needed
  "prune"   = drop entirely — stale tool output, superseded attempts, loops

Memory/skill actions:
  "upsert"   = add or update this item (merge with existing by content hash)
  "deprecate" = decrement evidence count; if at 0, remove from store

Summary max 200 tokens per entry. Prefer single-turn ranges when possible.
</compression>
```

The trailing instruction is designed so the model sees no special tokens —
just a user message with a JSON schema. The KV cache for everything before
this message is preserved.

---

## LLM Response Parsing

The C++ layer parses the single JSON response and applies all changes atomically:

### Classification → history transformation

| Tag | Action |
|-----|--------|
| `core` | Kept verbatim in `history_` |
| `context` | Replaced with an `ArchiveEntry` in the compressed context block |
| `prune` | Removed from `history_` |

The compressed context block is a synthetic system message inserted after the
real system prompt:

```
[system prompt]
...memory/skill block...

[system]
Compressed conversation context:
{
  "version": 1,
  "archive": [
    {"turns": "3-7", "summary": "investigated file structure..."}
  ],
  "facts": {
    "last_goal": "fix the bug in compressor.cpp"
  }
}
```

The real system prompt is preserved verbatim — KV cache for it stays valid
across the compression boundary.

### Memory/skill store updates

| Action | C++ behavior |
|--------|--------------|
| `upsert` | Content-hash lookup; if exists, increment evidence; if new, insert with evidence=1 |
| `deprecate` | Content-hash lookup; decrement evidence. If evidence ≤ 0, remove from store |

### Decay run

After applying all upserts/deprecations, run `decay_all()` on remaining items
in the store as usual.

---

## Budget Enforcement

The compressed output must fit within the compression budget:

| Budget | Fraction | Purpose |
|--------|----------|---------|
| `core` | 0.30 | Verbatim active turns |
| `archive` | 0.15 | Structured JSON context block |
| `headroom` | 0.50 | Model output space after compression |

If the LLM output violates the budget (e.g. too many core turns), the C++
layer walks backward from oldest core turns, promoting them to `context`
or `prune` until the budget fits. This is a hard C++ safety net.

---

## Error Handling

| Failure | Behavior | Rationale |
|---------|----------|-----------|
| LLM returns invalid JSON | Skip compression, log warning, continue | Never let middleware break the loop |
| LLM returns valid JSON with out-of-range turn indices | Clamp to valid range, log | Graceful degradation |
| Compression LLM call itself loops | Loop detection catches it, fall through to noop | Self-healing |
| Parse succeeds but produces empty history | Keep system prompt + last user message, log | Preserve at least the active request |

---

## File Map

| File | SRP |
|------|-----|
| `include/agent/compressor.h` | Ports: `CompressionGate`, `CompressionStrategy`; value types |
| `lib/compressor.cpp` | `CompressionPipeline`, `DefaultCompressionGate`, factory functions |
| `lib/compressor_request.cpp` | Builds the user message prompt for the LLM request |
| `lib/compressor_parser.cpp` | Parses the LLM JSON response into classifications, memories, skills |
| `lib/compressor_apply.cpp` | Applies classification to `history_` (prune/archive/keep), applies store mutations |
| `lib/compressor_scanner.cpp` | Loop detection + collapse pre-processing |
| `lib/experience.cpp` | `ExperienceExtractor` adapter — routes LLM-deprecated items to store |
| `lib/memory_store.cpp` | `JsonMemoryStore` — unchanged |
| `lib/memory_retriever.cpp` | `MemoryRetriever` — unchanged |

---

## Integration Points

### `Agent::compress_now()` (called by `/compress` command or automatic gate)

```cpp
CompressionResult Agent::compress_now() {
    // 1. Pre-process: collapse loops
    compressor_scanner::collapse_loops(history_);

    // 2. Build request prompt
    auto request = compressor_request::build(history_);

    // 3. Append as user message
    history_.push_back(request);

    // 4. Call LLM (same system prompt, no tools)
    Message reply = client_.chat(history_, /*tools=*/{}, &stats);

    // 5. Remove the request/response from history_
    history_.pop_back();  // request
    // (keep response for logging but don't store it permanently)

    // 6. Parse JSON response
    auto result = compressor_parser::parse(reply.content);

    // 7. Apply changes atomically
    compressor_apply::classify(history_, result.classification);
    compressor_apply::upsert_memories(memory_store_, result.memories);
    compressor_apply::upsert_skills(memory_store_, result.skills);
    compressor_apply::deprecate(memory_store_, result.skills);
    memory_store_->decay_all();
    memory_store_->save(experience_cfg_.store_path);

    // 8. Update stats
    return build_result(history_, result);
}
```

### `Agent::chat_once()` — automatic compression gate

```cpp
Message Agent::chat_once(...) {
    auto prompt_msgs = history_;
    if (gate_ && gate_->should_compress(history_, cfg_)) {
        // compress_now operates on the copy, not history_
        // (automatic compression is always non-destructive)
        compress_copy(prompt_msgs);
    }
    // ... LLM call, return ...
}
```

### `make_compressor()` signature (updated from current)

```cpp
std::unique_ptr<CompressionStrategy> make_compressor(
    LLMClient& client, const CompressionConfig& cfg);
```

Wiring in `main.cpp` and `tui.cpp` must pass `client_` to the factory.

---

## Testing Strategy

| Test | What it covers | How |
|------|---------------|-----|
| Loop scanner | Correctly identifies tool/text loops | Feed history with repeated tool calls, verify collapse |
| Request builder | Produces valid instruction message | Template check |
| JSON parser | Valid/invalid/malformed responses | Mock LLM output, verify parse-or-fallback |
| History apply | Core/context/prune correctly mutates history | Feed classified history, verify message counts |
| Store mutations | Upsert/deprecate modifies store correctly | Pre-populate store, apply changes, verify evidence counts |
| Budget enforcement | Overflowing core preserved by C++ fallback | Generate oversized classification, verify truncation |
| KV cache invariant | System prompt never changes | Mock LLM, assert system message identity pre/post compression |

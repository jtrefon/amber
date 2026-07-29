# Context Compression Architecture

- **Status:** Active design
- **Applies to:** `lib/` (core domain), `include/agent/` (ports)
- **Design patterns:** Strategy, Observer, Memento, Command, Null Object

---

## Problem

The agent loop appends every turn to an ever-growing context. At 262K tokens on a 4090 + 27B Q4_K_M GGUF, **a single context prefill takes 10-30 minutes**. Every turn that changes the system prompt or injects memories at the prefix triggers a full prefill — the agent spends more time loading than thinking.

But: appending new tokens to the **tail** of an existing context (extending the KV cache by 500-1000 tokens) takes ~1-2 seconds. The entire architecture is designed around this asymmetry.

---

## Core Insight: Prefill Is the Bottleneck, Not Token Count

Transformer inference has two phases:

**Prefill** — build KV cache for all input tokens. Cost: O(context_length²) for the first token.
**Generation** — extend KV cache token by token. Cost: O(context_length) per token.

For a 262K-context model:
- **Full prefill** = 10-30 minutes. The model processes every token through every attention layer once.
- **KV extension** (append 500 tokens) = ~1-2 seconds. Only the new tokens compute attention against the existing KV.
- **Generation** (produce 1000 tokens) = ~10-30 seconds. Each token extends the KV by one step.

The dominant cost is prefill. Every full rebuild of the 262K KV cache costs 10-30 minutes. The architecture ensures: **only one full prefill per compression cycle, zero system prompt changes during steps.**

### How other agents waste prefill

Systems like Hermes compress by spawning independent LLM calls, each with a different prompt. This creates N full prefills:

```
Call 1: [system_A][full history]  → prefill 262K → new KV
Call 2: [system_B][full history]  → prefill 262K → different KV
Call 3: [system_C][full history]  → prefill 262K → different KV
= 3 × 10-30 min = 30-90 minutes just for prefilling
```

### Our approach: zero prefills during compression

Every compression step appends to the SAME prefix. The KV cache for `[system][conv 1..N]` is built once and extended incrementally.

```
Step 1: [system][conv 1..N][CLASSIFY]       → extends KV by 400 tokens
Step 2: [system][conv 1..N][CLASSIFY][EXTRACT] → extends KV by another 400
Step 3: (C++ side, no LLM call)
Step 4: [system][compressed 2K-10K] → ONE prefill of compressed → 1 second
         ^-- unavoidable — the context changed
```

Total: 1 expensive prefill (paid once) + 1 cheap prefill (compressed context) + trivial KV extensions.

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│                    Compression cycle                                  │
│                                                                      │
│  ┌────────────┐    ┌───────────┐    ┌───────────┐    ┌───────────┐  │
│  │  Collapse   │    │  Step 1   │    │  Step 2   │    │  Step 3   │  │
│  │  Loops      │───▶│  Classify │───▶│  Extract  │───▶│  Assemble │  │
│  │  (C++, free)│    │  (LLM)    │    │  (LLM)    │    │  (C++)    │  │
│  └────────────┘    └─────┬─────┘    └─────┬─────┘    └─────┬─────┘  │
│                          │                │                │        │
│                    extends KV       extends KV         replaces     │
│                    by ~400 tok.     by ~400 tok.       context      │
│                          │                │                │        │
│                   ┌──────┴──────┐  ┌──────┴──────┐  ┌─────┴─────┐  │
│                   │ parse JSON  │  │ parse JSON  │  │ atomically│  │
│                   │ classify    │  │ memories +  │  │ swap      │  │
│                   │ turns       │  │ skills      │  │ context   │  │
│                   └─────────────┘  └─────────────┘  └───────────┘  │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### Prefix stability

The single most important invariant: **the system prompt NEVER changes during a session.**

Every memory/skill injection, every mode switch, every config change — they all go in a **separate message slot** after the system prompt, not appended to it. This ensures the KV cache for the system prompt is built once and reused for every LLM call in the session.

---

## Pre-processing: Loop Collapse (Free)

Before any LLM call, the C++ layer scans the conversation for consecutive identical tool calls (3+ repetitions of the same tool with the same arguments). These are replaced with a single `[loop collapsed]` note. This is pure C++ — zero token cost, zero KV cost.

## Step 1: Classify Turns

The compression engine builds a user message asking the LLM to classify each turn range as core, context, or prune. This message is appended to the existing conversation:

```
Request:
  [system]
  [conversation turns 1..N]
  [CLASSIFY INSTRUCTION]          ← new user message, ~400 tokens
```

The KV cache for `[system][conv 1..N]` is already built from the previous turn. Only the 400-token instruction computes new attention.

The LLM returns a JSON array of `{turns, tag, summary}` plus optionally continues with its normal assistant response (if the classification was embedded in a regular turn).

```
Response:
  [{"turns": "0-5", "tag": "prune", "summary": ""},
   {"turns": "6-8", "tag": "core", "summary": ""},
   ...]
```

### Classification prompt design

The prompt uses a work-state framework, not just tag names:

| Tag | When to use | What happens |
|-----|------------|-------------|
| `core` | Part of the current investigation. Active file, active search, recent decisions. | Kept verbatim in compressed output. |
| `context` | Supporting info that may be needed. Build config, project structure, workflows discovered. | Archived with one-line summary in JSON block. |
| `prune` | Completed investigation (bug found and fixed). Competing branch (tried approach A, moved to B). Dead-end file read. Loop. Stale tool output. | Removed entirely. If it's a completed investigation, the LLM should also create a memory entry summarizing the finding. |

## Step 2: Extract Memories and Skills

The extraction prompt is a separate request, appended AFTER step 1's response:

```
Request:
  [system]
  [conversation turns 1..N]
  [CLASSIFY INSTRUCTION]
  [CLASSIFY RESPONSE]
  [EXTRACT INSTRUCTION]           ← new user message, ~300 tokens
```

KV cache from step 1 is reused. Only the ~300 new tokens compute attention.

The LLM returns:

```json
{
  "memories": [
    {"name": "bug-fix-parser-null", "content": "Parser segfault on empty input
     fixed by null check in parse_body()", "tags": ["parser"], "action": "upsert"}
  ],
  "skills": [
    {"name": "run-tests", "content": "'make test' runs 150+ unit tests",
     "trigger_phrase": "test", "action": "upsert"}
  ]
}
```

### Why two steps instead of one combined request

| Approach | KV cost | Parse complexity | Error handling |
|----------|---------|-----------------|----------------|
| One combined JSON (classify + extract) | 1 extension, ~500 tokens | Complex multi-schema JSON. One schema error kills all. | All-or-nothing |
| Two focused requests | 2 extensions, ~400 tokens each | Each is a simple single-schema JSON. Independent testing. | Per-step: extraction failure doesn't lose classification |

Total KV extension difference: ~500 vs ~800 tokens (0.2% vs 0.3% of a 262K context). Negligible. The engineering benefits of separate, testable, independently-failing steps are decisive.

## Step 3: Assemble Compressed Context (C++ Side)

Using the classification from step 1 and the extraction from step 2:

```
apply_classification(history, classify_response)  → compressed history
apply_compression_result(extract_response)          → memory/skill upserts
enforce_minimum_context(compressed, before)         → keep last user msg
enforce_headroom(compressed, context_size)          → keep 25% free
context_.replace(compressed)                        → atomic swap
```

The `replace()` atomically swaps the context. The next LLM call builds KV for the compressed context (2K-10K tokens instead of 262K).

---

## Safety Nets

### Minimum context invariant

If the LLM classifies everything as "prune" (producing an empty compressed output), the system restores the last user message before replacing. The agent always retains: system prompt + archive block + last user message.

### Headroom enforcement

After compression, the total token count must leave at least 25% of the context window free for the next LLM response. If the LLM over-classifies as "core", the C++ layer walks backward from the oldest core messages and reclassifies them as "context" until the headroom is met.

### Cooldown

After a compression cycle, the gate is blocked for 20 turns. This prevents thrashing — no compression of the freshly compressed context while it's still well within the window.

---

## File Map

| File | Responsibility |
|------|---------------|
| `include/agent/compressor.h` | Ports: `CompressionGate`, `CompressionStrategy`; value types |
| `lib/compressor.cpp` | `CompressionPipeline`, `DefaultCompressionGate`, factory functions |
| `lib/compressor_request.cpp` | Builds classification and extraction instruction messages |
| `lib/compressor_parser.cpp` | Parses classification and extraction JSON responses |
| `lib/compressor_apply.cpp` | Applies classification to history, applies store mutations |
| `lib/compressor_scanner.cpp` | Loop detection + collapse (C++ side, zero token cost) |
| `lib/agent.cpp` | `Agent::compress_now()`, `Agent::apply_compression_result()`, automatic gate in `chat_once()` |
| `lib/memory_store.cpp` | `JsonMemoryStore` — persistence, scoring, decay |
| `lib/memory_retriever.cpp` | `MemoryRetriever` — relevance-scored injection |

---

## Error Handling

| Failure | Behavior | Rationale |
|---------|----------|-----------|
| Step 1 LLM call fails | Return original history unchanged | Safe fallback |
| Step 2 LLM call fails (step 1 succeeded) | Apply classification only. No memory ops. | Degraded-but-safe |
| Parse failure returns empty JSON | Pipeline returns pre-classification history | Never corrupt |
| LLM returns out-of-range turn indices | Clamp both `turn_start` and `turn_end` to `history.size() - 1` | Graceful |
| LLM returns all-prune classification | Minimum context invariant restores last user message | Never lose task |
| Compressed history exceeds headroom | C++ layer promotes oldest core → context until 25% free | Hard safety net |

---

## Testing Strategy

| Test | What it covers |
|------|---------------|
| Loop scanner | Identifies 3+ identical tool calls, short history no-op |
| Classification prompt | Produces valid user message, system prompt unchanged |
| JSON parser | Valid/invalid/malformed responses, edge cases |
| History apply | Core/context/prune correctly mutates history |
| Store mutations | Upsert/deprecate/conflict on memory store |
| Budget enforcement | Core overflow, headroom enforcement |
| Minimum context | All-prune classification preserves last user message |
| Integration | Full cycle: collapse → classify → extract → apply → replace → continue |

---

## Integration Points

### `Agent::compress_now()` (manual `/compress` command)

```cpp
CompressionResult Agent::compress_now() {
    auto before = context_.get_all();
    size_t msgs_before = before.size();
    size_t tokens_before = context_.token_count();

    // Step 0: collapse loops (C++ side)
    collapse_loops(before);

    // Step 1: classify (appends to tail, extends KV)
    Message classify_req = build_classify_request(before);
    before.push_back(classify_req);
    Message classify_resp = client_.chat(before, {});

    CompressionResponse cr = parse_classify_response(classify_resp.content);
    if (cr.segments.empty()) return {};  // safe fallback

    before.pop_back();  // remove the classify request

    // Step 2: extract (appends classify result, extends KV)
    // Use the compressed (post-collapse, post-classify) history
    auto post_classify = apply_classification(before, cr);
    Message extract_req = build_extract_request(cr);
    post_classify.push_back(extract_req);
    Message extract_resp = client_.chat(post_classify, {});

    CompressionResponse er = parse_extract_response(extract_resp.content);

    // Step 3: assemble
    apply_compression_result(er);
    enforce_minimum_context(post_classify, before);
    enforce_headroom(post_classify);

    context_.replace(post_classify);

    // stats
    CompressionResult r = ...;
    return r;
}
```

### `Agent::chat_once()` — automatic gate (appends classify to normal turn)

```cpp
Message Agent::chat_once(..., bool display) {
    auto prompt_msgs = context_.get_all();

    // Memory/skill injection (never touches system prompt — separate slot)
    if (retriever_ && store_version_changed_) {
        inject_knowledge(prompt_msgs);
        store_version_changed_ = false;
    }

    // Gate check
    if (gate_ && gate_->should_compress(context_, cfg_)) {
        auto classify_req = build_classify_request(prompt_msgs);
        prompt_msgs.push_back(classify_req);
        // Model responds with: [classification JSON][normal response]
    }

    // LLM call — extends KV from regular conversation + classify instruction
    Message reply = client_.chat_stream(prompt_msgs, tools, ...);

    // If gate was active, pre-parse the classification JSON from the reply
    if (gate_ && gate_->should_compress(context_, cfg_)) {
        auto [classify_json, text_response] = split_classify_response(reply.content);
        if (!classify_json.empty()) {
            apply_classification(context_.get_all(), parse_classify_response(classify_json));
            // store memory/skill extraction is deferred to chat_once's post-processing
            // or a separate hook
        }
        reply.content = text_response;  // strip the JSON, keep the text
    }

    return reply;
}
```

---

## References

- **Compression pipeline spec**: `docs/spec/compression/compression-pipeline.md`
- **Memory extraction spec**: `docs/spec/memory/extraction.md`
- **Memory store spec**: `docs/spec/memory/memory-store.md`
- **Skill operations spec**: `docs/spec/memory/skill-operations.md`
- **Deep audit**: `docs/deep-audit/context-system-analysis.md`

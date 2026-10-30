## Spec: Prompt caching (the wire-prefix contract)

### Purpose
Provider prompt caches are **token-prefix** caches: the provider reuses the
longest leading run of tokens it has already seen and bills only the rest. The
only thing that makes a cache hit possible is an unchanged prefix, and the only
thing that proves one happened is the provider's cache counters. This spec
defines which part of an amber request is allowed to be that prefix, what may
change, and how each protocol is told to cache it.

The contract lives **on the wire**, not in `Context`. `Context` being immutable
is necessary but not sufficient: a dialect can produce a different first token
from the same live context without touching it, and that is exactly the
regression this spec exists to prevent.

### Ownership
- **Projection**: `lib/agent.cpp` (`insert_prompt_blocks`,
  `collect_prompt_blocks`), `lib/dialect_openai.cpp` (`merge_system_content`),
  `lib/dialect_anthropic.cpp` (`build_chat_body`), `lib/dialect_gemini.cpp`.
- **Directives**: `lib/dialect_anthropic.cpp` (`mark_cacheable`,
  `mark_last_turn_cacheable`), `Config::prompt_cache_ttl`.
- **Telemetry**: `TokenUsage` (`include/agent/dialect.h`), `Stats`
  (`include/agent/llm.h`), `StreamDecoder`, `lib/http_transport.cpp`,
  `UsageSample`/`UsageTotals`, `tui/usage_report.h`.
- **Tests**: `tests/run_tests.cpp` (wire shape + usage parsing),
  `tests/dialect_anthropic_test.cpp` (body shape),
  `tests/agent_loop_test.cpp` (`agent_placed_injected_blocks_after_the_system_prompt_keeps_the_prefix`).

---

### The three tiers

A request is built in this order, and the order is the cache:

| Tier | Content | Changes between turns? | Position |
|------|---------|------------------------|----------|
| **1. Stable prefix** | system prompt (prompt files, environment card, tool prose, plugin `System` blocks), tool schemas | no (session-fixed) | first |
| **2. Conversation** | user turns, assistant replies, tool results, harness notices | append-only | middle |
| **3. Volatile injections** | session brief, activated skill bodies, plugin `Head`/`Tail` blocks, compression archive | yes | after it all |

Rules:

1. **Nothing in tier 1 may be recomputed from live state per request.** The
   environment card is probed once per session; a change there is a deliberate
   prefix reset, not a per-turn refresh.
2. **Tier 2 is append-only.** Rewriting history (compression, shaping) is a
   deliberate prefix reset and must be the only thing that does it.
3. **Tier 3 never enters tier 1.** It is rendered fresh each request and placed
   after the conversation, so its churn costs only its own tokens.
4. **A system-role message is tier 1 or the archive.** Every dialect hoists
   `role=system` into the leading block (OpenAI and Gemini need one leading
   system message, and strict GGUF templates reject a system message anywhere
   else). Therefore a block that changes between turns **must not be
   `role=system`**: it rides the user channel with `Message::harness` set, which
   is how the harness has always spoken.

### Protocol directives

| Protocol | Mechanism | What amber sends |
|----------|-----------|------------------|
| Anthropic Messages | explicit `cache_control` breakpoints (caching is opt-in) | `system` as a **block array**: the first block carries `{"type":"ephemeral"}`, injected system blocks follow it uncached; a second breakpoint is placed on the last user turn (a prompt or tool results) so the next request reads the conversation |
| OpenAI-compatible | automatic prefix caching | nothing; the stable prefix and append-only conversation do the work |
| Gemini | implicit caching | nothing (amber does not create `cachedContent` resources) |

Breakpoint budget: Anthropic allows 4; amber uses 2.

### TTL

`/set provider cache 5m|1h|off` (`Config::prompt_cache_ttl`, persisted in the
global config next to the provider):

- `5m` (default) — the protocol default; omit `ttl`.
- `1h` — the extended lifetime; `ttl: "1h"` on every breakpoint. A cache write
  costs more, but it survives the minutes a human takes between turns, which is
  the difference between hitting and missing on an interactive session.
- `off` — no breakpoints; the request shape is byte-identical to the
  pre-caching one. Useful for debugging and for gateways that reject the field.

### Telemetry (this is the gate)

Every dialect fills the counters the provider reports:

| Provider | Read | Write |
|----------|------|-------|
| OpenAI-compatible | `usage.prompt_tokens_details.cached_tokens` | — |
| Anthropic | `usage.cache_read_input_tokens` | `usage.cache_creation_input_tokens` |
| Gemini | `usageMetadata.cachedContentTokenCount` | — |

They flow `TokenUsage → Stats → UsageSample → UsageTotals` and surface in
`/usage` (`up … down … · cache … read / … write`). `-1` means "the provider did
not report it"; `0` means it reported and the request was **not** cached, which
is the regression signal. A long session reading `0` on most requests is a bug,
not a cheap session.

### Invariants (pinned by tests)

1. `anthropic_request_marks_the_stable_prefix_cacheable` — the first system block
   carries a breakpoint and the newest turn carries the moving one.
2. `anthropic_injected_blocks_sit_after_the_breakpoint` — an injected system
   block is after the cached block and uncached.
3. `anthropic_cache_ttl_follows_the_setting` — `1h` sets `ttl`, `off` produces
   the plain-string body, an unknown value is not guessed.
4. `agent_placed_injected_blocks_after_the_system_prompt_keeps_the_prefix` — a
   brief refresh between two turns does not change a byte of the cached block,
   and the volatile block is a user message after the conversation.
5. `anthropic_usage_reads_cache_counters`, `openai_usage_reads_cached_tokens`,
   `gemini_usage_reads_cached_tokens` — the counters reach `TokenUsage`.
6. `prompt_cache_ttl_roundtrips_in_the_global_config` — the setting persists and
   an unusable value warns instead of silently becoming a TTL.

### Known prefix resets (deliberate, one-time)

- **Compression** rewrites tier 2 and the archive; the stable prefix survives
  its breakpoint, so the re-prefill covers the conversation being rewritten, not
  the prompt being re-sent.
- **A system-prompt change** (plugin toggled, model switched, worktree adopted,
  day rolled over on the environment card) rewrites tier 1 by design.
- **A memory/skill/index update** changes a tier-1 system block: the cached
  block before it survives, the conversation behind it does not. These blocks
  are stable by construction (`MemoryRetriever` caches until the store version
  moves), so the cost is per update, not per turn.

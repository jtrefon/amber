## Spec: Prompt Assembly, injected blocks

### Purpose

Define what the model receives as *instructions* on every request: the base
system prompt, the blocks the harness injects for the turn (retrieved memories,
skill metadata, session brief), and the blocks plugins contribute, where each
one sits, in what order, and why.

The rule this spec exists to enforce: **an injected block is assembled after the
compression gate, never before it.** The compression rebuild replaces the prompt
copy, so anything injected earlier is discarded. That is not hypothetical, it
silently dropped retrieved memories on every compressing turn, which is exactly
when a long session needs them.

### Ownership

- **Source files**: `lib/agent.cpp` (`Agent::inject_prompt_blocks`, called from
  `chat_once`), `include/agent/agent.h` (`prompt_priority`),
  `lib/extensions.cpp` + `include/agent/extensions.h` (`PromptRegistry`, for
  plugin-contributed blocks).
- **Consumers**: `chat_once` (the only caller), plugins via
  `PromptBlockCapability`.
- **Test files**: `tests/agent_loop_test.cpp` (assembly, ordering, both turn
  types), `tests/agent_events_test.cpp` (plugin blocks).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | The sealed `Context` (read-only), plus agent-owned state: the memory retriever, the skill catalog, the brief store, and the shared `PromptRegistry` |
| **Output** | A **prompt copy**: the context messages with injected blocks added. The `Context` itself is never mutated. |
| **Ordering** | Head blocks are inserted immediately after the system prompt; tail blocks are appended after the conversation. Within each, ascending `prompt_priority` (ties keep insertion order). |
| **Error states** | A block that renders empty is omitted. A memory-retrieval failure yields no block, never a failed request. A plugin block that throws must not break the turn (the registry renders each block independently). |
| **Invariants** | See below. |
| **Thread safety** | Runs on the agent owner thread, like the rest of `chat_once`. The `PromptRegistry` is shared across windows, so blocks must be renderable from any agent. |

### Invariants

1. **After the gate.** Blocks are assembled once per request, after any
   compression rebuild. No call site injects into the prompt before the gate.
2. **The Context is never touched.** Blocks are written to the prompt copy.
   The hash chain and the single-owner rule are unaffected
   (`docs/spec/context/context-ownership-and-parallel-compression.md`).
3. **A block survives a compressing turn.** If a block appears in the prompt on
   an ordinary turn, it appears on a compressing turn. That equivalence is the
   property the tests pin, for every block.
4. **Deterministic for identical inputs.** The same context and the same agent
   state produce the same prompt. A block that varies per turn belongs at the
   tail, so the server's KV prefix survives up to its position.
5. **One ordering authority.** The order is `prompt_priority`, declared beside
   the assembly. It is not reconstructed from the position of each injection
   site.
6. **Plugin blocks are ordered by the registry**, which sorts by (priority,
   registration), and follow the core blocks.

### Why core blocks are not registry entries

`PromptRegistry` is owned by the runtime and **shared by every window**, while
the memory retriever, skill catalog and brief store are **per-agent**. Moving a
core block into the shared registry would put one window's state into another
window's prompt. The registry is for app-wide contributions; core blocks are
assembled by the agent that owns their state. Both are merged in one ordered
pass so the model sees a single coherent sequence.

---

### Scenarios

#### [PA-01] A block survives a compressing turn

- **Given**: an agent with a retrievable memory, and a gate that fires
- **Input**: one turn, with compression rebuilding the prompt
- **Expected**: the memory text reaches the model
- **Regression guard**: `agent_keeps_injected_blocks_when_compression_fires`

#### [PA-02] A block is present when compression does not fire

- **Given**: the same agent, with a gate that never fires
- **Input**: one turn
- **Expected**: the memory text reaches the model, the fix for [PA-01] must not
  cost anything on the ordinary path
- **Regression guard**: `agent_injects_in_memory_blocks_without_compression`

#### [PA-03] Documented order

- **Given**: memory, a session brief and a plugin block all present
- **Input**: one turn
- **Expected**: base system prompt first; memory (head, priority 100) next;
  brief (tail, 950) and plugin block (tail, 1000) after the conversation, in
  that order
- **Regression guard**: `agent_orders_injected_blocks_as_documented`,
  `agent_places_injected_blocks_after_the_system_prompt`

#### [PA-04] Non-compressing turns are unchanged

- **Given**: the same agent, memory + brief + plugin block
- **Input**: one turn with no compression
- **Expected**: the prompt is byte-for-byte what it was before this spec's
  implementation, the fix's blast radius is compressing turns only
- **Regression guard**: verified by comparing dumped prompts across the change
  (recorded in `docs/plugin-framework-tracker.md`); the ordering test pins the
  shape going forward

---

### Cross-references

- **Depends on**: `context/context-ownership-and-parallel-compression.md` (the
  sealed stack and the prompt-copy rule), `compression/compression-pipeline.md`
  (the rebuild this spec orders around)
- **Depended on by**: `plugins/plugin-framework.md` (plugin prompt blocks),
  `agent-loop/core-loop.md` (`chat_once` as a step of the turn)
- **Test coverage**: `tests/agent_loop_test.cpp` (PA-01..03),
  `tests/agent_events_test.cpp` (plugin block rendering)

### Revision history

| Date | Reason |
|------|--------|
| 2026-09-11 | Initial spec, single assembly point after the compression gate; documents the memory-block regression and why core blocks are not registry entries |

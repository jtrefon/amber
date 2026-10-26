## Spec: Compression — Turn Classification

- **Status:** current (reconciled with the code in CC-12, 2026-10-26)
- **Source:** `lib/compressor_apply.cpp` (`apply_classification`, `tag_turns`,
  `protect_active_task`, `archive_json_for`), `lib/compressor_parser.cpp`
- **Pipeline:** `compression-pipeline.md`

### Purpose

Turn the classifier's per-range tags into a new message list: `core` stays
verbatim, `context` becomes an archive entry, `prune` disappears — with the
system prompt, the active tail and provider-valid tool pairing guaranteed
regardless of what the model said.

### Contract

| Dimension | Detail |
|---|---|
| Input | History snapshot + `CompressionResponse{segments, summary, memory_ops, skill_ops, brief}` + optional `CompressionConfig` |
| Output | New message list: system prompt, surviving core turns, the compressed-context message, the protected tail |
| Error states | Empty history or empty segments → the input is returned unchanged |
| Purity | No mutation of the input; the caller rebuilds the `Context` |

### The index space

The classifier's `turns` values are **message indices** — the same numbers the
classify instruction's transcript manifest shows it (`compression-pipeline.md`).
`tag_turns` clamps both ends to `history.size() - 1`; later segments overwrite
earlier tags; unclassified messages default to `core` (the safe direction).

### What apply guarantees

1. **System prompt.** Index 0 is restored even if the classifier tagged it
   `prune` (segment ranges are applied from index 1).
2. **Protected tail.** `protected_tail_start` returns the newest real user turns
   that fit the tail budget, always including the final prompt. Everything from
   there on is forced to `core`; harness notices do not count as user turns.
3. **Minimum context.** If no real user message survives, the last one is
   restored. The budget passes re-apply this guarantee after their drops.
4. **Tool pairing.** `sanitize_tool_pairs` drops orphaned results and stubs
   calls whose results were pruned, so the rebuilt history is provider-valid.
5. **Archive.** One entry per contiguous `context` run:
   `{"turns": "a-b", "summary": "..."}`. Entries from a previous compression are
   carried forward and the archive is capped (32 entries / ~800 tokens, oldest
   merged into `earlier work, compressed`).
6. **Compressed-context message.** A single `system` message carrying
   `{archive, summary}` — the classifier's work-state summary is the semantic
   channel the next turn continues from. Nothing else is written: `type`,
   `version` and `facts` had no reader.
7. **No-op passes.** `apply_classification` always emits the compressed-context
   message; the pipeline compares totals and returns the original when the pass
   did not reduce (invariant I1), so a no-op costs nothing.

### Scenarios (pinned by name in `tests/run_tests.cpp`)

| ID | Case | Test |
|---|---|---|
| TC-01 | all core | `apply_classification_all_core` |
| TC-02 | prune + archive | `apply_classification_prunes_and_archives` |
| TC-03 | empty response | `apply_classification_empty` |
| TC-04 | non-contiguous context runs → separate entries | `apply_classification_context_creates_archive_entry` |
| TC-05 | parse failure → empty response → pipeline returns the original | `parse_compression_response_invalid_json`, `compression_pipeline_failure_returns_history_unchanged` |
| TC-06 | system prompt survives a `prune`/`context` tag on index 0 | `apply_classification_preserves_system_when_classifier_tags_as_prune` |
| TC-07 | previous archive carried forward, old message consumed | `apply_classification_carries_previous_archive` |
| TC-08 | archive capped and merged | `archive_entries_are_capped_and_summarised` |
| TC-09 | no write-only payload | `compressed_context_message_has_no_write_only_fields` |
| TC-10 | tail survives a misclassification | `compression_never_drops_the_active_user_message` |

### Cross-references

- `compression-pipeline.md` — the pass that calls this
- `loop-collapse.md` — the other reduction, applied after assembly
- `docs/spec/memory/extraction.md` — memory/skill ops carried by the same response

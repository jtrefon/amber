## Spec: Compression — Turn Classification

### Purpose
Apply the LLM's classification response to the conversation history: keep
`core` turns verbatim, archive `context` turns as JSON summaries, and discard
`prune` turns. Produces the compressed output message vector.

### Ownership
- **Source files**: `lib/compressor_apply.cpp` (`apply_classification()` — lines 13–84, `build_compression_result()`), `lib/compressor_parser.cpp` (`parse_compression_response()`)
- **Test files**: `tests/run_tests.cpp` — 4 apply tests (lines 1657–1717)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | History + `CompressionResponse{segments, memory_ops, skill_ops}` |
| **Output** | Compressed history: core verbatim, context archived, pruned removed, plus `compressed_context` system message |
| **Error states** | Empty/no segments → history unchanged. Parse failure → post-collapse history returned. |
| **Invariants** | See below. |

### Invariants

1. Core turns are preserved verbatim in original order.
2. Context turns are removed and their content summarised into `archive[]`.
3. Prune turns are removed entirely — no trace in output.
4. The archive JSON is inserted as a single `role: "system"` message.
5. Archive entries are contiguous ranges; non-contiguous context spans produce separate entries.

---

### Scenarios

#### [TC-01] All core — no change

- **Given**: All turns classified as core
- **Input**: `apply_classification(history, segments_all_core)`
- **Expected**: History unchanged. No archive block added.
- **Regression guard**: `apply_classification_all_core` test.

#### [TC-02] Prune and archive

- **Given**: Mixed tags: 2 core, 3 context, 1 prune
- **Input**: 6-turn history
- **Expected**: Prune removed. Context turns removed, grouped into one archive entry. Core preserved. Archive block inserted as system message.
- **Regression guard**: `apply_classification_prunes_and_archives` test.

#### [TC-03] Empty classification — no-op

- **Given**: Empty segments array
- **Input**: `apply_classification(history, empty_response)`
- **Expected**: Returns history unchanged.
- **Regression guard**: `apply_classification_empty` test.

#### [TC-04] Non-contiguous context spans

- **Given**: Context at turns 1-2 and 4-5, core at 3
- **Input**: 5-turn history
- **Expected**: Two archive entries: `archive[0]` for turns 1-2, `archive[1]` for turns 4-5. Core turn 3 preserved between them.

#### [TC-05] Parse failure — JSON extraction fails

- **Given**: LLM returns non-JSON text
- **Input**: `parse_compression_response("I don't understand compression")`
- **Expected**: Returns empty `CompressionResponse`. Pipeline returns post-collapse history.
- **Regression guard**: `parse_compression_response_invalid_json` test.

---

### Cross-references

- **Depends on**: `compression/compression-pipeline.md`, `compression/loop-collapse.md`
- **Depended on by**: `memory/extraction.md` (memory/skill ops extracted from same LLM response)
- **Test coverage**: `tests/run_tests.cpp`: all 4 apply_classification tests + 3 parse tests

### Known gaps

1. **`turn_start` not clamped** — `apply_classification` clamps `turn_end` to `history.size()-1` but does NOT clamp `turn_start`. Out-of-range start causes undefined `tags[i]` access.
2. **Archive segment detection** — Non-contiguity check compares `seg.turn_start` against loop index `i`. This only works for single-turn segments. Multi-turn segments with non-contiguous ranges may not be detected correctly.
3. **`CompressionBudget` not enforced** — Core/archive/headroom fractions declared but never checked.

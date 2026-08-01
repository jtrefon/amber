## Spec: Search Backend — Semantic (Lexical Index)

### Purpose
Provide a dependency-free lexical-semantic search using TF-IDF weighting with
a hashing trick for embedding. Builds an in-memory index per `(root, glob)`
pair, ranks results by cosine similarity.

### Ownership
- **Source files**: `tools/search/semantic_backend.cpp` (126 lines), `tools/search/semantic_index.cpp` (109 lines), `include/agent/semantic_helpers.h`
- **Test files**: `tests/run_tests.cpp` — semantic search test (line 634)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Query string, root directory, optional glob, max results |
| **Output** | `vector<SearchHit>` with rank score |
| **Error states** | Empty index → empty results. |
| **Invariants** | See below. |

### Invariants

1. Index is built once per `(root, glob)` pair and cached — NOT invalidated on file changes.
2. Embedding uses 1024-dim hashing trick with sign from LSB of term hash.
3. IDF weight: `log(N / df) + 1.0`.
4. Same exclusions as grep: `.amber`, `.git`, `third_party/`.
5. Lines longer than 4096 chars are truncated before indexing.

---

### Scenarios

#### [SB-01] Build index then search

- **Given**: Workspace with files
- **Input**: `backend.search("build system")`
- **Expected**: Index built. Results ranked by relevance. Most relevant (highest cosine similarity) returned first. Score in output.
- **Regression guard**: `search_semantic_backend_ranks_relevant` test.

#### [SB-02] Cache hit — no rebuild

- **Given**: Same root/glob as previous search
- **Input**: Second `search()` call
- **Expected**: No file walk. Returns from cached index immediately.
- **On failure**: Index rebuilt every search.

#### [SB-03] Stale index after file change

- **Given**: File modified after first search
- **Input**: Same root/glob
- **Expected**: Index NOT rebuilt. Stale results returned. **Known gap** — no mtime tracking.
- **On failure**: Index rebuilt detecting change.

#### [SB-04] Empty index

- **Given**: No matching files in workspace
- **Input**: `backend.search("anything")`
- **Expected**: Empty results. No crash.
- **On failure**: Crash on empty index.

---

### Cross-references

- **Depends on**: `search-backends/backend-selection.md`
- **Depended on by**: `tools/search-tool.md`
- **Test coverage**: `tests/run_tests.cpp`: `search_semantic_backend_ranks_relevant` (634)

### Known gaps

1. **No mtime tracking** — Index is never invalidated when files change. Class comment claims it detects mtime changes but implementation only checks root/glob strings.
2. **Full index in memory** — Every line of every matching file stored with a 1024-dim double vector. Memory-intensive for large codebases.
3. **`walk()` uses `popen()`** — Calls `find` via shell, same platform-dependency issue as grep backend.
4. **`matches_glob()` only handles `*`** — No `?`, `[...]`, or `**` pattern support.

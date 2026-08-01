## Spec: Search Tool

### Purpose
Search file contents using either `grep` (fast, regex-based) or a dependency-free
semantic index (TF-IDF with hashing trick). The tool is read-only, relaxed about
path confinement, and dispatches to the chosen backend via the `mode` parameter.

### Ownership
- **Source files**: `tools/search_tool.cpp` (SearchTool, 122 lines), `tools/search/grep_backend.cpp` (GrepBackend, 89 lines), `tools/search/semantic_backend.cpp` (SemanticBackend, 126 lines), `tools/search/semantic_index.cpp` (walk, tokenize, embed, cosine, 109 lines), `include/agent/search_backend.h` (SearchBackend, SearchHit)
- **Factory**: `include/agent/tools.h` → `make_search_tool()`
- **Test files**: `tests/run_tests.cpp` — 4 search tests (lines 605–667)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `{"pattern": <string (max 256)>, "path": <optional string>, "glob": <optional string>, "mode": <optional "grep"|"semantic">, "max": <optional int, default 200>}` |
| **Output** | `ToolResult{ok, output, error, meta}`. Output is one hit per line. Meta: `{"hits": N, "mode": "grep"|"semantic"}`. |
| **Error states** | Missing pattern → error. Pattern too long (>256 chars) → error. Backend failure → empty results (not error). |
| **Invariants** | See below. |
| **Thread safety** | Stateless factory functions. Semantic backend caches index per `(root, glob)` pair — concurrent calls with same root/glob share the same cached index (read-only after initial build). |

### Invariants

1. Pattern is required and must not exceed 256 characters.
2. Backend defaults to `grep` for any `mode` other than `"semantic"`.
3. Path confinement is lenient: if `Workspace::confine()` fails, the raw (unconfined) path is used anyway (read-only).
4. The `.amber`, `.git`, and `third_party/` directories are always excluded from search.
5. Grep mode excludes binary files via `-I` flag.
6. Semantic mode caches the index and does NOT detect file changes between searches.

---

### Scenarios

#### [ST-01] Grep mode — basic search

- **Given**: A file containing "hello world"
- **Input**: `{"pattern": "hello", "mode": "grep"}`
- **Expected**: `ok=true`. Output: `"path/to/file:1:hello world\n"`. Meta: `{"hits": 1, "mode": "grep"}`.
- **Regression guard**: `search_grep_backend` test.

#### [ST-02] Grep mode — no matches

- **Given**: No file contains the pattern
- **Input**: `{"pattern": "nonexistent_string_xyz"}`
- **Expected**: `ok=true`. Output: `"no matches (grep)"`. Meta: `{"hits": 0}`.

#### [ST-03] Grep mode — shell injection resistance

- **Given**: Pattern contains shell metacharacters
- **Input**: `{"pattern": "'; cat /etc/passwd; '"}`
- **Expected**: Commands are not executed. Output is safe literal search. The `shell_quote()` function wraps in single quotes with `'\''` escaping for embedded quotes.
- **Regression guard**: `search_grep_backend_resists_shell_injection` test.

#### [ST-04] Semantic mode — relevance ranking

- **Given**: Two files: one contains "build system using cmake", other contains "system call interrupted"
- **Input**: `{"pattern": "build system", "mode": "semantic"}`
- **Expected**: Both files matched. The "build system using cmake" line ranks higher (higher cosine similarity with "build system"). Output prepends `(score=0.XX)` per hit.
- **Regression guard**: `search_semantic_backend_ranks_relevant` test.

#### [ST-05] Semantic mode — index caching

- **Given**: First search builds index; second search with same root/glob
- **Input**: Two `semantic_backend.search()` calls with same root+glob
- **Expected**: Second call does NOT re-index. Returns from cache immediately. Index is NOT rebuilt on file changes.
- **On failure**: Index rebuilt on every search, or stale index silently used after file changes.

#### [ST-06] Path outside workspace

- **Given**: Workspace root = `/project`, path = `/etc`
- **Input**: `{"pattern": "root", "path": "/etc"}`
- **Expected**: `Workspace::confine()` rejects `/etc` → falls back to raw `/etc`. Search runs on `/etc`. Results returned.
- **Note**: Search is deliberately lenient for read-only operations.

#### [ST-07] Missing pattern

- **Input**: `{"mode": "grep"}` (no pattern)
- **Expected**: `ok=false`, `error="missing 'pattern'"`.

#### [ST-08] Pattern too long

- **Input**: `{"pattern": "<257 chars>", "mode": "grep"}`
- **Expected**: `ok=false`, `error="pattern too long (257 chars)"`.

#### [ST-09] Glob filter

- **Given**: Workspace with `.cpp` and `.md` files
- **Input**: `{"pattern": "TODO", "glob": "*.cpp"}`
- **Expected**: Only `.cpp` files searched. Hits only from those files.
- **On failure**: All files searched regardless of glob.

#### [ST-10] Max results capped

- **Given**: 500 matching files
- **Input**: `{"pattern": "a", "max": 10}`
- **Expected**: Exactly 10 results returned. Both backends respect the `max` parameter.

#### [ST-11] Semantic mode — empty index

- **Given**: Workspace with no matching files
- **Input**: `{"pattern": "anything", "mode": "semantic"}`
- **Expected**: Output: `"no matches (semantic)"`. Meta: `{"hits": 0}`.

---

### Cross-references

- **Depends on**: `search-backends/grep-backend.md`, `search-backends/semantic-backend.md`, `workspace/path-confinement.md`
- **Depended on by**: `agent-loop/tool-dispatch.md`
- **Test coverage**: `tests/run_tests.cpp`: `search_grep_backend` (605), `search_grep_backend_resists_shell_injection` (621), `search_semantic_backend_ranks_relevant` (634), `search_tool_mode_switch` (647)

### Known gaps

1. **Path fallback on confinement failure** — Inconsistent with read/write tools which reject out-of-workspace paths. Search silently works on unconfined paths.
2. **No regex validation** — Invalid regex in grep mode silently returns no results (stderr redirected to `/dev/null`).
3. **Semantic index never invalidates** — File changes between searches are not detected. The `ensure_index()` check only compares root and glob strings.
4. **Backend selection checks mode string** — `mode == "semantic"` should check `backend->name()` for extensibility.
5. **Output format differs between backends** — Grep uses `path:line:text`, semantic uses `path:line (score=X) text`. The model must parse both.

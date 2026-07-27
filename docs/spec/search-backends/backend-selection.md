## Spec: Search Backend — Selection & Registration

### Purpose
Dispatch search queries to the appropriate backend based on the `mode` argument.
Also register all default tools at startup via `register_default_tools()`.

### Ownership
- **Source files**: `tools/search_tool.cpp` (mode dispatch), `lib/tools_default.cpp` (registration)
- **Test files**: `tests/run_tests.cpp` — `search_tool_mode_switch` test (line 647)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `mode` argument from SearchTool: `"grep"` or `"semantic"` |
| **Output** | `SearchBackend*` allocated per call (stateless factory, semantic backend caches internally) |
| **Error states** | Unknown mode → grep fallback |
| **Invariants** | See below. |

### Invariants

1. Any `mode` other than `"semantic"` defaults to grep.
2. Backend is created fresh on each `execute()` call.
3. Semantic backend caches its index internally per `(root, glob)` pair.
4. Grep backend is stateless — no caching.

---

### Scenarios

#### [BS-01] Mode dispatch — grep

- **Given**: `mode="grep"`
- **Input**: `search("pattern", root, "", 200, "grep")`
- **Expected**: `make_grep_backend()` called. Results in `path:line:text` format.
- **Regression guard**: `search_tool_mode_switch` test.

#### [BS-02] Mode dispatch — semantic

- **Given**: `mode="semantic"`
- **Input**: `search("pattern", root, "", 200, "semantic")`
- **Expected**: `make_semantic_backend()` called. Results ranked with `(score=X)` prefix.
- **Regression guard**: `search_tool_mode_switch` test.

#### [BS-03] Unknown mode — grep fallback

- **Given**: `mode="unknown"`
- **Input**: Any query
- **Expected**: Falls back to grep backend.
- **On failure**: No results returned.

#### [BS-04] Tool registration

- **Given**: `register_default_tools(registry, jobs)`
- **Expected**: All tools registered: read, write, search, bash, process_start, process_read, process_stop.
- **On failure**: Missing tool causes agent to fail on model request.

---

### Cross-references

- **Depends on**: `search-backends/grep-backend.md`, `search-backends/semantic-backend.md`
- **Depended on by**: `tools/search-tool.md`
- **Test coverage**: `tests/run_tests.cpp`: `search_tool_mode_switch` (647)

### Known gaps

1. **Backend selection checks mode string** — `mode == "semantic"` instead of checking `backend->name()`. Adding a third backend requires editing the format logic.
2. **No health check** — Backends cannot report unavailability before search.

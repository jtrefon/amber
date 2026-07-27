## Spec: Search Backend — Grep

### Purpose
Execute `grep -rnIE` over workspace files with exclusion filters and parse
the output into structured `SearchHit` results.

### Ownership
- **Source files**: `tools/search/grep_backend.cpp` (89 lines)
- **Test files**: `tests/run_tests.cpp` — grep search tests (lines 605–632)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Query string, root directory, optional glob, max results |
| **Output** | `vector<SearchHit>` with path, line_no, line content |
| **Error states** | Invalid regex → empty results (stderr silent). Shell injection → guarded by `shell_quote()`. |
| **Invariants** | See below. |

### Invariants

1. All queries are shell-quoted via single quotes with `'\''` escaping for embedded quotes.
2. Binary files are excluded via `-I`.
3. `.amber`, `.git`, and `third_party/` directories are excluded.
4. Per-file match limit: 10,000 matches (`--max-count=10000`).
5. stderr is redirected to `/dev/null` — grep errors are silent.

---

### Scenarios

#### [GB-01] Basic search — matches found

- **Given**: File with "hello world"
- **Input**: `backend.search("hello", root)`
- **Expected**: One hit: `path:1:hello world`. Score = hit count (grep returns in natural order).
- **Regression guard**: `search_grep_backend` test.

#### [GB-02] No matches

- **Given**: No file contains the pattern
- **Input**: `backend.search("xyzzy")`
- **Expected**: Empty vector.
- **On failure**: Error or crash.

#### [GB-03] Glob filter

- **Given**: `.cpp` and `.md` files
- **Input**: `backend.search("TODO", root, "*.cpp")`
- **Expected**: Only `.cpp` files searched.
- **Regression guard**: `search_grep_backend` test (glob parameter).

#### [GB-04] Shell injection resistance

- **Given**: Query with shell metacharacters
- **Input**: `"; cat /etc/passwd; '"`
- **Expected**: Query is shell-quoted. No command injection. Safe literal search.
- **Regression guard**: `search_grep_backend_resists_shell_injection` test.

#### [GB-05] Invalid regex

- **Given**: Pattern is not valid regex
- **Input**: `"["` (unclosed bracket)
- **Expected**: grep returns non-zero, stderr to /dev/null. Empty results returned.
- **On failure**: Crash or error message returned to user.

---

### Cross-references

- **Depends on**: `search-backends/backend-selection.md`
- **Depended on by**: `tools/search-tool.md`
- **Test coverage**: `tests/run_tests.cpp`: `search_grep_backend` (605), `search_grep_backend_resists_shell_injection` (621)

### Known gaps

1. **Shell injection surface via `popen()`** — `shell_quote()` handles single quotes but the command passes through `/bin/sh`.
2. **Path parsing fragile** — Splits on first `:` — paths containing colons are mis-parsed.
3. **`--max-count=10000` hardcoded** — Not configurable.

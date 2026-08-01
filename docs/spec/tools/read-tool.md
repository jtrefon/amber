## Spec: Read Tool

### Purpose
Provide a controlled, paginated file read that respects workspace boundaries
and feeds content back to the LLM in a structured envelope. The LLM uses this
to inspect files, verify edits, and gather context.

### Ownership
- **Source files**: `tools/read_tool.cpp`
- **Factory declaration**: `include/agent/tools.h` (`make_read_tool()`)
- **Envelope formatting**: `lib/agent_helpers.cpp` (`format_tool_envelope()`)
- **Test files**: `tests/run_tests.cpp` (3 test blocks)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | JSON object: `{"path": <string>, "offset": <optional int, default 1>, "limit": <optional int, default 200>}` |
| **Output** | `ToolResult{ok, output, error, meta}` where `meta = {"lines": N, "total": M, "more": bool}` |
| **Error states** | 4 distinct modes (see below) |
| **Invariants** | See below |
| **Thread safety** | Stateless — safe to call from any thread. Multiple instances may execute in parallel via dispatch layer. |

### Invariants

1. Every read is confined to the workspace root. No file outside the workspace is ever read.
2. Output line numbers are 1-based, human-readable (`<line>:\t<content>`).
3. Pagination offset is 1-based (line 1 = first line).
4. Every result includes a trailing hint: `[more lines available...]` or `[end of file: N lines]`.
5. The LLM always receives the feedback envelope with `status`, `meta`, and content when `ok=true`.
6. Line count never exceeds `limit` (default 200, minimum 1).
7. Binary files are NOT detected — any file that `std::getline` can read is returned.

---

### Scenarios

#### [RD-01] Successful read, first page

- **Given**: A 10-line file `foo.txt`
- **Input**: `{"path": "foo.txt", "offset": 1, "limit": 3}`
- **Expected**: `ok=true`. Output contains lines 1-3 with line numbers. Meta: `{"lines": 3, "total": 10, "more": true}`. Output ends with `[more lines available: 7 remaining; pass offset=4 to continue]`.
- **On failure**: Return `ok=false` with descriptive error.

#### [RD-02] Successful read, mid-page

- **Given**: Same file, continuing from [RD-01]
- **Input**: `{"path": "foo.txt", "offset": 4, "limit": 3}`
- **Expected**: Lines 4-6. Meta: `{"lines": 3, "total": 10, "more": true}`. Hint at end.
- **Edge case**: `offset` must be ≤ `total` lines. If offset > total, output is empty line(s) with `more=false`.

#### [RD-03] Successful read, last page (end of file)

- **Given**: Same file
- **Input**: `{"path": "foo.txt", "offset": 9, "limit": 50}`
- **Expected**: Lines 9-10. Meta: `{"lines": 2, "total": 10, "more": false}`. Output ends with `[end of file: 10 lines]`.

#### [RD-04] Missing path argument

- **Input**: `{"limit": 5}` (no `"path"`)
- **Expected**: `ok=false`, `error="missing 'path'"`.
- **Envelope**: `[tool=read ... status=error meta={}]\nERROR: missing 'path'\n[end]`

#### [RD-05] Path escapes workspace

- **Given**: Workspace root set to `/tmp/amber_ws`
- **Input**: `{"path": "/etc/passwd"}`
- **Expected**: `ok=false`, `error="path escapes workspace root (/tmp/amber_ws): /etc/passwd"`.
- **Edge cases**: Absolute paths to system files, `../` traversal, symlink chains to outside.
- **Invariant**: Path resolution happens BEFORE any file I/O.

#### [RD-06] File does not exist

- **Input**: `{"path": "nonexistent.txt"}`
- **Expected**: `ok=false`, `error="cannot open: <resolved_path>"`.
- **Recovery**: LLM is expected to verify path and retry.

#### [RD-07] File is a directory

- **Input**: `{"path": "."}`
- **Expected**: `ok=false`, `error="cannot open: <resolved_path>"`. (`std::ifstream` fails on directories.)
- **Recovery**: LLM is expected to list directory contents instead.

#### [RD-08] Permission denied

- **Given**: File with `chmod 000`
- **Input**: `{"path": "locked.txt"}`
- **Expected**: `ok=false`, `error="cannot open: <resolved_path>"`.
- **Note**: Read tool does not distinguish permission-denied from not-found. Both produce the same error.

#### [RD-09] Negative offset / limit

- **Input**: `{"path": "foo.txt", "offset": -5, "limit": -1}`
- **Expected**: Both clamped to 1. Equivalent to `offset=1, limit=1`.

#### [RD-10] Offset past end of file

- **Given**: 10-line file
- **Input**: `{"path": "foo.txt", "offset": 100, "limit": 5}`
- **Expected**: `ok=true`, output is empty string. Meta: `{"lines": 0, "total": 10, "more": false}`. Hint: `[end of file: 10 lines]`.

#### [RD-11] Parallel reads (multiple files)

- **Given**: Files `a.txt` and `b.txt`
- **Input**: Two tool calls in the same LLM turn
- **Expected**: Both execute concurrently via dispatch. Each returns its own `ToolResult`. Results are independent — one failure does not cancel the other.
- **Test note**: This is dispatch behaviour, not read-tool behaviour. Read tool is stateless and thread-safe by construction.

#### [RD-12] Relative path resolution

- **Given**: Workspace root = `/project`
- **Input**: `{"path": "src/main.cpp"}`
- **Expected**: Reads `/project/src/main.cpp`. The `confine()` call prepends the workspace root.

#### [RD-13] The feedback envelope

- **Given**: Any successful read
- **Expected**: Before the LLM sees the result, `format_tool_envelope()` wraps it:
  ```
  [tool=read args={"path":"foo.txt","offset":1,"limit":200} status=ok meta={"lines":3,"total":10,"more":true}]
  1:	first line
  2:	second line
  3:	third line
  [more lines available: 7 remaining; pass offset=4 to continue]
  [end]
  ```
- **Invariant**: The envelope is immutable — the tool does not control its format.

---

### Cross-references

- **Depends on**: `workspace/path-confinement.md`, `agent-loop/tool-dispatch.md`
- **Depended on by**: `docs/spec/INDEX.md` (tools category)
- **Test coverage**: `tests/run_tests.cpp`: `read_tool_basic_and_pagination` (lines 466-491), `read_tool_missing_path_errors` (lines 493-498), `read_write_tools_reject_paths_outside_workspace` (lines 564-578)

### Known gaps

1. **Binary file detection** — No guard. Binary files are read as text. Content is sanitised upstream by `utf8_sanitize()`.
2. **File size limit** — Protection is only via the `limit` parameter. If the model requests `limit=999999` on a large file, the entire file is read into memory and output. A maximum-byte cap should exist.
3. **Error granularity** — All three failure modes (missing arg, confinement, cannot open) produce `ok=false` with a string error. The LLM cannot distinguish "file not found" from "permission denied" from "is a directory" without parsing the error string.

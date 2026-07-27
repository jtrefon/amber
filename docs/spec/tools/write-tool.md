## Spec: Write Tool

### Purpose
Create and patch files in the workspace using exact-match `old`→`new` edits.
The write tool is the primary mechanism for the agent to modify source code,
configuration files, and documentation.

### Ownership
- **Source files**: `tools/write_tool.cpp` (104 lines)
- **Factory**: `include/agent/tools.h` → `make_write_tool()`
- **Test files**: `tests/run_tests.cpp` — 3 write-tool tests (lines 500–578)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | JSON: `{"path": <string>, "edits": [{"old": <string>, "new": <string>}, ...]}`. `old=""` means "full overwrite" (create or replace entire file). |
| **Output** | `ToolResult{ok, output, error, meta}` where `meta = {"applied": N, "path": P}` |
| **Error states** | 5 distinct modes (see below) |
| **Invariants** | See below. |
| **Thread safety** | Stateless — safe to call from any thread. Edits are applied sequentially to in-memory copy, then flushed to disk atomically (single `std::ofstream` truncate). |

### Invariants

1. Every path is confined to the workspace root before any read or write.
2. Edits are applied sequentially to mutable content — edit N+1 operates on the result of edit N.
3. `old=""` replaces the entire file content with `new` (create or full overwrite).
4. Non-empty `old` must appear exactly once in the file — `find()` returns the first match only.
5. On error, the file is NOT modified (error returned before any write).
6. If file does not exist, `content` starts empty. `old=""` must be used to create it.

---

### Scenarios

#### [WT-01] Create new file

- **Given**: File does not exist
- **Input**: `{"path": "new.txt", "edits": [{"old": "", "new": "Hello world"}]}`
- **Expected**: `ok=true`. File created with content `"Hello world"`. Meta: `{"applied": 1, "path": "..."}`.
- **On failure**: `"cannot write: <path>"` (permissions, read-only filesystem).

#### [WT-02] Overwrite existing file

- **Given**: File with existing content `"foo"`
- **Input**: `{"path": "f.txt", "edits": [{"old": "", "new": "bar"}]}`
- **Expected**: `ok=true`. File truncated and rewritten to `"bar"`.
- **Warning**: No safety mechanism — full overwrite is silent.

#### [WT-03] Patch existing file (exact-match replace)

- **Given**: File with `"Hello world"`
- **Input**: `{"path": "f.txt", "edits": [{"old": "world", "new": "there"}]}`
- **Expected**: `ok=true`. Content becomes `"Hello there"`.
- **On failure**: `"edit 0 not applied: 'old' not found"`.

#### [WT-04] Multiple edits in sequence

- **Given**: File `"a\nb\nc\n"`
- **Input**: `{"edits": [{"old": "a", "new": "x"}, {"old": "c", "new": "z"}]}`
- **Expected**: Both applied. Content: `"x\nb\nz\n"`. Meta: `{"applied": 2}`.
- **On failure**: Edit 1 succeeds, edit 2 fails → error returned. File NOT written (no partial write). **But**: the in-memory content is mutated by edit 1 before edit 2 runs. If edit 2 fails, the mutation leaks — this is in-memory only, the file is unmodified because the write hasn't happened yet.

#### [WT-05] Edit with empty new (deletion)

- **Given**: File `"Hello world"`
- **Input**: `{"edits": [{"old": " world", "new": ""}]}`
- **Expected**: Content becomes `"Hello"`.
- **On failure**: `"edit 0 not applied: 'old' not found"`.

#### [WT-06] Missing path

- **Input**: `{"edits": [...]}` (no path)
- **Expected**: `ok=false`, `error="missing 'path'"`.

#### [WT-07] Missing edits array

- **Input**: `{"path": "f.txt"}` (no edits)
- **Expected**: `ok=false`, `error="missing non-empty 'edits'"`.

#### [WT-08] Empty edits array

- **Input**: `{"path": "f.txt", "edits": []}`
- **Expected**: `ok=false`, `error="missing non-empty 'edits'"`.

#### [WT-09] Path escapes workspace

- **Given**: Workspace root `/project`
- **Input**: `{"path": "/etc/passwd", "edits": [...]}`
- **Expected**: `ok=false`, `error="path escapes workspace root (/project): /etc/passwd"`.
- **On failure**: File outside workspace is modified.

#### [WT-10] File is read-only or no permission

- **Given**: File with `chmod 444` or directory without write permission
- **Input**: Any valid edit
- **Expected**: `ok=false`, `error="cannot write: <path>"`.

#### [WT-11] Consecutive identical edits

- **Given**: File `"a a"`
- **Input**: `{"edits": [{"old":"a","new":"b"}, {"old":"a","new":"c"}]}`
- **Expected**: After edit 1: `"b b"`. After edit 2: first `'b'` is at position 0, which is `'b'` not `'a'`. `find("a")` returns position 2 (the second `'a'`). Content becomes `"b c"`. The model must account for shifted offsets.

---

### Cross-references

- **Depends on**: `workspace/path-confinement.md`, `agent-loop/tool-dispatch.md`
- **Depended on by**: `docs/spec/INDEX.md` (tools category)
- **Test coverage**: `tests/run_tests.cpp`: `write_tool_create_then_patch` (lines 500–526), `write_tool_missing_old_fails` (lines 528–541), `read_write_tools_reject_paths_outside_workspace` (lines 564–578)

### Known gaps

1. **No overwrite safety** — `old=""` silently destroys existing file without backup, diff preview, or confirmation. A model error loses data.
2. **No atomic write** — `std::ofstream(path, std::ios::trunc)` truncates before writing. Crash between truncation and write loses the file. Should write to `.tmp` then `std::rename`.
3. **Single-match only** — `content.find(old_s)` returns first occurrence. Cannot replace all occurrences of a pattern.
4. **No encoding validation** — Reads/writes raw bytes. No UTF-8, line-ending, or binary detection.
5. **Sequential dependency** — Edit N+1 operates on mutated result of edit N. Model must account for offset shifts. Could surprise the model.
6. **Edit tool is NOT a separate tool** — It is part of WriteTool. The INDEX spec entry for `edit-tool.md` should reference `write-tool.md`.

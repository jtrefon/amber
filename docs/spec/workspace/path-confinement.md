## Spec: Workspace Path Confinement

### Purpose
Ensure all file operations (read, write, edit) stay within the workspace root
directory. Uses lexical path normalisation with slash-terminated prefix
matching to prevent `../` traversal and sibling-directory prefix collisions.

### Ownership
- **Source files**: `lib/workspace.cpp` (90 lines), `include/agent/workspace.h` (50 lines)
- **Test files**: `tests/run_tests.cpp` — `workspace_confines_relative_and_rejects_escape` (544), `read_write_tools_reject_paths_outside_workspace` (564)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | User-supplied path (relative or absolute) |
| **Output** | Resolved absolute, normalised path within workspace, or error |
| **Error states** | Empty path → rejected. `../` traversal → rejected. Absolute path outside root → rejected. |
| **Invariants** | See below. |
| **Thread safety** | Function-local static root — `reset_root()` is NOT thread-safe. `confine()` reads root which is set once at startup. |

### Invariants

1. Root resolution order: cached > `$AMBER_WORKSPACE` > `cwd`.
2. Root is always absolute and normalised.
3. Confinement is purely lexical — no filesystem access during path resolution.
4. Trailing `/` on root prevents `"/tmp/amber_ws2"` being considered inside `"/tmp/amber_ws"`.
5. `confine()` is called BEFORE any file I/O — no file is opened before the confinement check.
6. Empty paths are always rejected.

---

### Scenarios

#### [PC-01] Relative path resolved to workspace

- **Given**: Workspace root = `/project`
- **Input**: `confine("src/main.cpp", resolved, err)`
- **Expected**: `ok=true`. `resolved = "/project/src/main.cpp"`.
- **Regression guard**: `workspace_confines_relative_and_rejects_escape` test.

#### [PC-02] Normalised relative path

- **Given**: Workspace root = `/project`
- **Input**: `confine("./x/../y.txt", resolved, err)`
- **Expected**: `ok=true`. `resolved = "/project/y.txt"`.
- **On failure**: Double `..` not collapsed.

#### [PC-03] Traversal escape rejected

- **Given**: Workspace root = `/tmp/amber_ws`
- **Input**: `confine("../../etc/passwd", resolved, err)`
- **Expected**: `ok=false`. `error = "path escapes workspace root (/tmp/amber_ws): ../../etc/passwd"`.
- **Regression guard**: `workspace_confines_relative_and_rejects_escape` test.

#### [PC-04] Absolute path outside workspace

- **Given**: Workspace root = `/tmp/amber_ws`
- **Input**: `confine("/etc/passwd", resolved, err)`
- **Expected**: `ok=false`.
- **Regression guard**: `read_write_tools_reject_paths_outside_workspace` test.

#### [PC-05] Sibling prefix collision rejected

- **Given**: Workspace root = `/tmp/amber_ws`
- **Input**: `confine("/tmp/amber_ws2/x", resolved, err)`
- **Expected**: `ok=false` — trailing `/` on root prevents `amber_ws2` from matching `amber_ws`.
- **Regression guard**: `workspace_confines_relative_and_rejects_escape` test.

#### [PC-06] Empty path

- **Given**: Any root
- **Input**: `confine("", resolved, err)`
- **Expected**: `ok=false`, `error = "empty path"`.

#### [PC-07] Same path as root

- **Given**: Workspace root = `/project`
- **Input**: `confine("/project", resolved, err)`
- **Expected**: `ok=true`. `resolved = "/project"` (exact match, not prefix).

---

### API: Workspace::relative()

```cpp
static std::string relative(const std::string& path);
```

Strips the workspace root prefix from an absolute path, returning a workspace-
relative path. Used by tools (read, search) to display compact file paths in
output summaries rather than full absolute paths.

- If `path` does not start with the workspace root, returns `path` unchanged.
- Returns `"."` when `path` equals the root exactly.
- Never throws.

### Cross-references

- **Depends on**: `workspace/security-model.md`
- **Depended on by**: `tools/read-tool.md`, `tools/write-tool.md`, `tools/search-tool.md`
- **Test coverage**: `tests/run_tests.cpp`: `workspace_confines_relative_and_rejects_escape` (544), `read_write_tools_reject_paths_outside_workspace` (564)

### Known gaps

1. **No symlink resolution** — Confinement is purely lexical. A symlink inside the workspace pointing outside is NOT detected.
2. **Function-local static root is not thread-safe** — `set_root()` cannot be called concurrently with `confine()`.
3. **`AMBER_WORKSPACE` is read once** — Changing the env var after first `confine()` call has no effect (cached).

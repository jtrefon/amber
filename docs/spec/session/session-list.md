## Spec: Session — List

### Purpose
Enumerate saved sessions with metadata for the TUI session browser. Uses a
cached index (`index.json`) or falls back to scanning the sessions directory.

### Ownership
- **Source files**: `lib/session.cpp` (`SessionStore::list()`, `scan_directory()`, `rebuild_index()`)
- **Test files**: No direct tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `SessionStore::list()` |
| **Output** | `vector<SessionMeta>` sorted by `updated_ms` descending |
| **Error states** | Empty directory → empty list. |
| **Invariants** | See below. |

### Invariants

1. Results are sorted by `updated_ms` descending (most recent first).
2. `index.json` and `workspace.json` are excluded from session listing.
3. Cache is invalidated after any `save()` or `remove()`.
4. When cache is invalid, every session file is fully parsed for metadata (O(n) JSON parses).

---

### Scenarios

#### [SL-01] List with valid cache

- **Given**: `index.json` exists and is current
- **Input**: `store.list()`
- **Expected**: Returns cached metadata list. No file scanning.
- **On failure**: Incorrect or stale list.

#### [SL-02] List with invalid cache

- **Given**: `index.json` deleted (after save)
- **Input**: `store.list()`
- **Expected**: `scan_directory()` iterates all `.json` files, loads each to extract metadata, returns sorted list.
- **On failure**: Performance issue with many sessions (each one fully parsed).

#### [SL-03] Empty directory

- **Given**: No sessions saved
- **Input**: `store.list()`
- **Expected**: Empty vector returned.
- **On failure**: Crash or error.

---

### Cross-references

- **Depends on**: `session/save-load.md`
- **Depended on by**: `tui/event-loop.md` (session browser)
- **Test coverage**: No direct tests.

### Known gaps

1. **Full parse of every session on miss** — `scan_directory()` parses every JSON file to extract metadata. For 100+ sessions this is O(n) full parses.
2. **Index cache is always stale** — `save()` deletes the index rather than updating it.
3. **No pagination** — All sessions returned at once regardless of count.

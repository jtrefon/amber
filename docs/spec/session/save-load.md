## Spec: Session — Save/Load

### Purpose
Persist and restore conversation sessions as JSON files in `.amber/sessions/`.
Each session captures full message history, model, timestamps, and metadata.

### Ownership
- **Source files**: `lib/session.cpp` (`SessionStore::save()`, `load()`, `remove()`), `include/agent/session.h`
- **Test files**: No direct session tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `Session` struct with messages + metadata |
| **Output** | JSON file at `<workspace>/.amber/sessions/<id>.json` |
| **Error states** | File not found → load returns empty. Corrupt JSON → load returns empty. |
| **Invariants** | See below. |

### Invariants

1. Each session gets a unique `id` based on timestamp + atomic counter.
2. Title is derived from first user message (first line, ≤40 chars + ellipsis).
3. Save is atomic: writes to `.tmp` then `std::rename`.
4. `updated_ms` is bumped on every save.
5. Index file (`index.json`) is deleted on every save — forces lazy rebuild on next `list()`.

---

### Scenarios

#### [SS-01] Save new session

- **Given**: Session with messages
- **Input**: `store.save(session)`
- **Expected**: File created at `<workspace>/.amber/sessions/<id>.json`. `id` populated. `created_ms` and `updated_ms` set. Index file deleted.
- **On failure**: File not written.

#### [SS-02] Load existing session

- **Given**: Saved session file
- **Input**: `store.load(id)`
- **Expected**: Session returned with all messages, metadata intact.
- **On failure**: Corrupt JSON → empty session (no crash).

#### [SS-03] Remove session

- **Given**: Existing session
- **Input**: `store.remove(id)`
- **Expected**: File deleted. Index also removed.
- **On failure**: `remove()` returns false for non-existent ID.

#### [SS-04] Workspace state save/load

- **Given**: Multi-window TUI state
- **Input**: `store.save_workspace(state)` then `store.load_workspace()`
- **Expected**: Windows (session_id, title, prompt_history) preserved across sessions.
- **On failure**: Workspace state lost.

---

### Cross-references

- **Depends on**: `session/session-list.md`
- **Depended on by**: `tui/event-loop.md` (autosave on Done/Quit)
- **Test coverage**: No direct tests.

### Known gaps

1. **Index file deleted on every save** — Forces full scan on next list. Index is never incrementally updated.
2. **Title derivation is simplistic** — First line of first user message, capped at 40 chars. No LLM-generated titles.

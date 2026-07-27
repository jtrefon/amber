# Spec Template

Every spec file in this directory follows this structure. This is the contract
between the feature and the rest of the system — if the code and the spec
disagree, the code is wrong.

---

## Spec: [Component Name]

### Purpose
_Why this exists. What problem it solves. One crisp paragraph._

### Ownership
- **Source files**: `path/to/impl.cpp`, `path/to/header.h`
- **Test files**: `tests/foo_test.cpp`

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | What the component receives (types, preconditions) |
| **Output** | What it guarantees to produce (types, format, postconditions) |
| **Error states** | Every error mode, typed, with recovery path |
| **Invariants** | Things that must *always* hold — if they don't, it's a bug |
| **Thread safety** | Which thread(s) may call this, what locks are held |

---

### Scenarios

Each scenario is a single, independently testable behaviour.

#### [SC-##] Short descriptive name

- **Given**: starting state / context
- **Input**: specific action, event, or argument
- **Expected**: what must happen (output, side-effect, state change)
- **On failure**: what error is produced and how the system recovers

#### [SC-##] Another scenario

...

---

### Cross-references

- **Depends on**: `docs/spec/other-spec.md`
- **Depended on by**: `docs/spec/dependent-spec.md`
- **Test coverage**: `tests/file_test.cpp` (TEST blocks: Foo, Bar, Baz)

---

### Revision history

| Date | Reason |
|------|--------|
| YYYY-MM-DD | Initial spec |

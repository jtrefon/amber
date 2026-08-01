## Spec: Authored Skill Files — Format & Discovery

### Purpose

Define the on-disk format of authored skills and how amber discovers them.
Authored skills are `SKILL.md` packages following the **Agent Skills open
standard** (`agentskills.io`), so files written for Claude Code, Codex, Gemini
CLI, or installed from the `skills.sh` marketplace work unchanged when placed in
an amber skill directory. This spec covers the format subset amber parses, the
tolerant frontmatter parser, the directory scanner, scope resolution, and the
opt-in interop gate.

### Ownership

- **Source files** (target): `include/agent/skill_file.h` (parser + scanner),
  `lib/skill_file.cpp`
- **Test files** (target): `tests/skill_file_test.cpp`
- **Spec status**: design — implementation tracked in `docs/skills-tracker.md`.

---

### Directory conventions

| Scope | Root | Contents |
|-------|------|----------|
| Project | `<workspace>/.amber/skills/` | Team-shared playbooks, git-committed |
| Global | `~/.config/amber/skills/` | Personal library, cross-project |
| Interop (opt-in) | `<workspace>/.claude/skills/`, `<workspace>/.codex/skills/` | Skills authored for other agents |

Each skill is a **directory** whose name is the kebab-case skill name,
containing at minimum a `SKILL.md` file:

```
<name>/
├── SKILL.md          # required: YAML frontmatter + markdown body
├── scripts/          # optional: executable helpers (run via existing tools only)
├── references/       # optional: documentation loaded on demand
└── assets/           # optional: templates, static resources
```

The directory name is the canonical skill name. `SKILL.md` is case-sensitive.

---

### SKILL.md format

A `SKILL.md` is two parts: a `---`-delimited YAML frontmatter block, then a
Markdown body.

#### Frontmatter fields

| Field | Required | Rules |
|-------|----------|-------|
| `name` | yes | kebab-case (`^[a-z0-9-]+$`), must match the parent directory name |
| `description` | yes | 1–2 sentences; "Use when …" trigger guidance; `>`-folded values supported |
| `license` | no | License name (e.g. `apache-2.0`); stored, not enforced |
| `compatibility` | no | Environment requirements (intended product, system packages); stored |
| `metadata` | no | Arbitrary key/value map (author, version, category, …); stored as JSON |
| `allowed-tools` | no | **Parsed and ignored.** Capability claims in skill text are untrusted — see `workspace/security-model.md` |

Unknown frontmatter keys are tolerated and ignored (forward compatibility).

#### Body

The Markdown body holds the skill's instructions: when to use, step-by-step
procedure, examples, edge cases. Guidance:

- Keep `SKILL.md` under **500 lines**; move detail to `references/`.
- Reference bundled files by **relative path from the skill root**
  (`references/guide.md`, `scripts/extract.py`).
- The body is loaded whole on activation, up to `skills_body_budget_tokens`
  (default 5000). Oversized bodies are rejected, not truncated.

---

### Frontmatter parser contract

Amber has no YAML dependency and does not add one. The parser handles the
**common subset** of the standard's frontmatter, tolerantly:

- Input: the first `---`-delimited block at the top of `SKILL.md`.
- Produces a `SkillMeta { name, description, license, compatibility,
  metadata, body }` — or a parse failure.
- Supports single-line values (`name: foo`) and `>`-folded block values for
  `description` (the form used by most real skills).
- `metadata:` subkeys are collected as a JSON object; other unknown top-level
  keys are ignored.
- `name` is validated against kebab-case; a mismatch with the directory name is
  **not** a parse failure but a discovery warning (directory name wins).

**Failure semantics:** a `SKILL.md` with malformed or missing frontmatter makes
the skill **undiscoverable** — it is excluded from the catalog with a warning.
It never crashes the scan, never breaks other skills, and never truncates the
body. `metadata` parse errors degrade to empty metadata, not exclusion.

---

### Directory scanner contract

| Dimension | Detail |
|-----------|--------|
| **Input** | A root directory (or a list, in scope-precedence order) |
| **Output** | Ordered list of `SkillFile { name, path, scope, meta, enabled }` |
| **Traversal** | Recursive, one level deep per skill directory (a skill dir contains `SKILL.md` at its root; nested skill dirs are not scanned) |
| **Exclusions** | Non-directory entries; dirs without `SKILL.md`; hidden entries; symlinks resolved per `workspace/path-confinement.md` |
| **Ordering** | Scan roots in precedence order: project → global → interop; within a root, lexicographic by dir name |
| **Dedup** | First occurrence of a name wins (project shadows global); later scopes skipped for that name |
| **Errors** | Unreadable `SKILL.md` → excluded with warning. Missing root dir → skipped (not an error) |

**Interop gate:** `.claude/skills` and `.codex/skills` are scanned only when
`skills_interop = true` (config or `/set skills interop on`). Off by default.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Skill directories, config (`skills_interop`) |
| **Output** | `SkillMeta` per discoverable skill; ordered `SkillFile` list |
| **Error states** | Malformed frontmatter → undiscoverable + warning. Dir/name mismatch → warning, dir wins. Unreadable file → excluded + warning. |
| **Invariants** | See below. |
| **Thread safety** | Parser is a pure function (no state). Scanner runs at session start and on `refresh` only; single-threaded. |

### Invariants

1. **Directory name is canonical.** The scan keys skills by their directory
   name; frontmatter `name` must agree or the frontmatter value is ignored with
   a warning.
2. **Kebab-case enforced.** Names failing `^[a-z0-9-]+$` are excluded with a
   warning (they are unroutable — the model cannot reliably type them).
3. **One skill per directory.** Nested skill directories are not scanned.
4. **Unknown fields never break discovery.** Forward-compatible by design.
5. **Interop is never on by default.**
6. **Symlinks and escapes** follow workspace confinement rules; a skill
   directory resolving outside its scan root is skipped.

---

### Scenarios

#### [SF-01] Minimal valid skill

- **Given**: `<workspace>/.amber/skills/run-tests/SKILL.md` with frontmatter `name: run-tests`, `description: Run the unit suite. Use when the user asks to run tests.` and a short body
- **Input**: Scan project root
- **Expected**: `SkillFile { name="run-tests", path=<dir>, scope=project, meta populated, enabled=true }`. Discoverable.
- **On failure**: Skill missing from catalog.

#### [SF-02] Folded description

- **Given**: `description: >` followed by two indented lines
- **Input**: Parse
- **Expected**: `description` is the folded single-line value. No parse error.
- **On failure**: Description empty; skill degraded.

#### [SF-03] Malformed frontmatter excluded

- **Given**: `SKILL.md` with no `---` block, or unparseable frontmatter
- **Input**: Parse + scan
- **Expected**: Skill excluded from catalog. Warning logged. Other skills unaffected.
- **On failure**: Scan aborts or skill truncated into the catalog.

#### [SF-04] Directory/name mismatch

- **Given**: Directory `foo-bar` contains frontmatter `name: baz-qux`
- **Input**: Parse + scan
- **Expected**: Warning logged; `foo-bar` used as the canonical name.
- **On failure**: Wrong name routed; model cannot find it.

#### [SF-05] Scan precedence — project shadows global

- **Given**: `deploy` exists in both `<workspace>/.amber/skills/` and `~/.config/amber/skills/`
- **Input**: Scan all roots
- **Expected**: Project `deploy` listed; global `deploy` skipped for the name.
- **On failure**: Both listed; conflicting instructions.

#### [SF-06] Interop gate off by default

- **Given**: `.claude/skills/` populated, `skills_interop` unset (false)
- **Input**: Scan
- **Expected**: No interop skills discovered.
- **On failure**: Third-party skills loaded without consent.

#### [SF-07] Unknown frontmatter keys ignored

- **Given**: Frontmatter with `name`, `description`, plus `metadata: {author: x}` and an unknown `custom-field: y`
- **Input**: Parse
- **Expected**: Known fields populated; `metadata` captured; `custom-field` ignored. No failure.
- **On failure**: Parser rejects valid future fields.

#### [SF-08] Non-kebab-case name excluded

- **Given**: Directory `My_Skill/`
- **Input**: Scan
- **Expected**: Excluded with warning (unroutable name).
- **On failure**: Skill advertised under a name the model can't target reliably.

#### [SF-09] Missing root is not an error

- **Given**: No `<workspace>/.amber/skills/` directory
- **Input**: Scan
- **Expected**: Empty result for that scope. No error.
- **On failure**: Session start fails on absent dir.

---

### Cross-references

- **Depends on**: `skills/agent-skills.md` (terminology, scope model, budgets), `workspace/path-confinement.md` (symlink/escape handling), `config/file-config.md` (`skills_interop`)
- **Depended on by**: `skills/skill-catalog.md` (consumes `SkillFile`/`SkillMeta`)
- **External**: Agent Skills open specification — `agentskills.io/specification`
- **Test coverage**: `tests/skill_file_test.cpp` (see `docs/skills-tracker.md`)

### Known gaps

1. **YAML subset only** — The parser handles the common subset (`name`,
   `description`, scalar + folded values, `metadata`). Exotic YAML (anchors,
   inline lists in `description`, multiline `|` blocks) is not supported and
   degrades to a parse failure → undiscoverable. Acceptable: real-world
   `SKILL.md` files use the subset.
2. **One-level skill depth** — The standard permits nested structure; amber
   scans only one skill-directory level per root. Deeper nesting is not
   discovered.
3. **No marketplace integration** — Discovery is directory-based; there is no
   client for `skills.sh` or any registry. Users install skills into the scanned
   dirs themselves.

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Initial spec |

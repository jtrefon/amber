## Spec: Skill Catalog — Union View, Tools, and Curation

### Purpose

Define the `SkillCatalog` — the runtime union view of every discoverable skill
(authored at all scopes + interop + learned) — and the surfaces that operate on
it: the `read_skill` / `write_skill` / `list_skills` tools the model can call,
and the `/set skills` / `/get skills` command subtree the user controls. The
catalog is the single access point for the progressive-disclosure tiers defined
in `skills/agent-skills.md`.

### Ownership

- **Source files** (target): `include/agent/skill_catalog.h`, `lib/skill_catalog.cpp` (catalog + overrides); `tools/skill_tools.cpp` (read/write/list tools); TUI wiring in `tui/tui_input.cpp`
- **Test files** (target): `tests/skill_catalog_test.cpp`
- **Spec status**: design — implementation tracked in `docs/skills-tracker.md`.

---

### Architecture

```
                    ┌──────────────────────────────────────────────┐
                    │               SkillCatalog                    │
                    │                                                │
   scan roots ─────►│  name → SkillEntry (union, dedup, precedence) │
   (project, global,│  { name, scope, origin, meta, body_cache }    │
    interop, learned)│                                                │
                    │  SkillOverrides (persisted enable/disable/    │
                    │   block, survives re-scan)                    │
                    │                                                │
   tools ──────────►│  read_skill │ write_skill │ list_skills        │
   /set skills ────►│  create │ delete │ export │ refresh │ show     │
                    └──────────────────────────────────────────────┘
```

---

### Ports

#### `SkillCatalog`

| Dimension | Detail |
|-----------|--------|
| **`discover()`** | Re-scans all scan roots (project → global → interop, gated) + learned store; applies overrides; rebuilds the union view. Returns the discovery block (ordered `name: description` lines, capped by `skills_max_discovery`). |
| **`lookup(name)`** | Returns the `SkillEntry` (metadata + cached body if loaded) or null. Applies precedence and override filtering. |
| **`read_body(name)`** | Loads and caches the `SKILL.md` body, enforcing `skills_body_budget_tokens`. Returns body or error. |
| **`apply_overrides(overrides)`** | Accepts a new `SkillOverrides` map and persists it to the overrides file. |
| **Caching** | Body cache is name-keyed, invalidated on `discover()`. Overrides file read at construction and after each apply. |
| **Thread safety** | `discover()`, `read_body()`, `apply_overrides()` run on the agent thread only; the UI reads a snapshot copy. |

#### `SkillOverrides`

Persisted JSON at `<workspace>/.amber/skills.json` (project) and
`~/.config/amber/skills.json` (global). Merged at load (project wins on name
collisions). Schema:

```json
{
  "run-tests":        {"state": "enable",  "note": "team standard"},
  "obsolete-workflow":{"state": "disable", "note": "superseded by deploy-v2"},
  "malicious-skill":  {"state": "block",   "note": "untrusted author"}
}
```

| State | Effect |
|-------|--------|
| `enable` | Force-include, even if a higher-precedence shadow exists. |
| `disable` | Excluded from discovery and activation. Persists across re-scans and reinstalls. |
| `block` | Like `disable`, plus recorded provenance (author/source) for audit. Recommended for untrusted third-party skills. |

There is no explicit `enable`-list file; absence of an override is the default
state.

---

### Tools (model-accessible)

Tools are ordinary `Tool` implementations (see `include/agent/tool.h`),
registered alongside the built-ins.

| Tool | Read-only | Approval | Behavior |
|------|-----------|----------|----------|
| `read_skill` | yes | never | Activates a skill: loads body (budget-capped), caches it, harness appends it as a message slot on the next prompt copy. Unknown name → `ToolResult{ok=false, error}`. |
| `list_skills` | yes | never | Returns the discovery block (same as the injected metadata), optionally filtered by `name` substring or `origin`. |
| `write_skill` | no | always | Authors a `SKILL.md` (see Authoring). Side-effecting; gated by approval and write mode. |

**`read_skill` schema**

```
parameters:
  name: { type: string, description: "Skill name to activate" }
```

**`write_skill` schema**

```
parameters:
  name:        { type: string, description: "kebab-case skill name" }
  description: { type: string, description: "1-2 sentence trigger guidance" }
  body:        { type: string, description: "Markdown instructions" }
  scope:       { type: string, enum: [project, global], default: project }
```

**Authoring rule:** `write_skill` and `/set skills create` are
**explicit-request only**. The agent never authors a skill unless the user asked
it to ("save this as a skill"). Learned extraction is the automatic path;
authoring is the deliberate one.

---

### Curation commands (`/set skills` / `/get skills`)

Subtree under the existing `set`/`get` command nodes (agent-internal state, like
`/set policy`). Design follows `nested-commands.md` `CommandNode` model.

```
set skills                               — Skill settings frame
  interop <on|off>                       — Scan .claude/skills, .codex/skills
  create <name> [--global]               — Author a new skill (opens editor)
  delete <name> [--global]               — Remove a skill (project by default)
  export <name>                          — Graduation: learned skill → global authored SKILL.md
  refresh                                — Re-scan roots, rebuild discovery
  show [name]                            — List skills + origins; details for one
  enable <name> | disable <name> | block <name>
                                         — Persist override (survives re-scan)
get skills [name]                        — Read current skill state (same as show)
```

**Flags and defaults:**

- `--global` selects the global scope for `create`/`delete`; default is project.
- `refresh` is the only way to re-scan — no automatic background scans.
- `export <name>` reads a learned skill from the experience store and writes a
  global `SKILL.md`, then re-scans. The learned skill stays in the store but is
  suppressed by precedence (authored wins).

**Scope mapping for command output** (`get skills` / `show`):

| Line | Meaning |
|------|---------|
| `project · authored · run-tests · enabled` | Project `SKILL.md`, override state |
| `global · authored · deploy · enabled` | Global `SKILL.md` |
| `interop · authored · gh-flow · enabled` | Opt-in interop skill |
| `project · learned · deploy · suppressed` | Learned skill shadowed by authored `deploy` |

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Scan roots, config, user commands, model tool calls |
| **Output** | Discovery block; activated bodies; authored/overrides files; command output |
| **Error states** | Unknown skill → `read_skill` error. Oversized body → activation rejected. `export` of nonexistent learned skill → error. Unwritable target dir → authoring error. |
| **Invariants** | See below. |
| **Thread safety** | Agent-thread mutation; UI reads snapshots. |

### Invariants

1. **The catalog is the only path.** Progressive disclosure, precedence, and
   overrides apply to every skill regardless of how it is reached (session
   start, `read_skill`, `list_skills`, `/get skills`).
2. **Authored skills are not executed.** `write_skill`/`create` write files;
   `read_skill` injects text. No new execution surface is introduced.
3. **Overrides persist.** `enable`/`disable`/`block` are written to the
   overrides file immediately and survive re-scans and reinstalls.
4. **Authoring is explicit.** Neither the agent nor any command authors a skill
   without a user request.
5. **`export` is one-way.** It copies learned → authored; it never writes back
   into the experience store and never modifies the learned skill's score.
6. **Budget caps hold.** Discovery is capped by `skills_max_discovery`, bodies by
   `skills_body_budget_tokens`. Exceeding fails closed.
7. **No cross-project leak.** Learned skills are read from the project store
   only; global authored skills are the only cross-project surface.

---

### Scenarios

#### [SK-01] Session start union discovery

- **Given**: 3 project-authored, 2 global-authored, 1 interop (gate off), 2 learned
- **Input**: `Agent` construction → `catalog.discover()`
- **Expected**: Union has 5 authored + 2 learned = 7 entries. Discovery block lists 7 lines. Interop absent.
- **On failure**: Wrong count; interop leaked.

#### [SK-02] read_skill activates + caches

- **Given**: Skill `code-review-checklist` discovered
- **Input**: `read_skill("code-review-checklist")`
- **Expected**: Body loaded (under budget), cached, appended as a message slot on next prompt copy. Second `read_skill` is served from cache (no re-read).
- **On failure**: Body re-read every call; prompt slot duplicated.

#### [SK-03] read_skill unknown name

- **Given**: No skill named `nope`
- **Input**: `read_skill("nope")`
- **Expected**: `ToolResult{ok=false, error="unknown skill: nope"}`. Nothing injected.
- **On failure**: Empty success; model confused.

#### [SK-04] write_skill authors a project skill

- **Given**: User asks "save this as a skill" → agent calls `write_skill`
- **Input**: `write_skill({name:"run-tests", description:"...", body:"...", scope:"project"})`
- **Expected**: Approval prompt shown; on approve, `<workspace>/.amber/skills/run-tests/SKILL.md` written, `discover()` re-run, discovery block updated next turn.
- **On failure**: No approval → tool denied, nothing written.

#### [SK-05] write_skill never unsolicited

- **Given**: Agent is mid-task; no user request to save a skill
- **Input**: Model calls `write_skill` anyway
- **Expected**: Approval prompt surfaces the write; user denies. The harness prompt instructs the model to only author on explicit request (see `prompts/skills.md`).
- **On failure**: Skill silently authored; surprise file.

#### [SK-06] disable override suppresses across scopes

- **Given**: `obsolete-workflow` disabled in project overrides; skill on disk
- **Input**: `discover()` after `/set skills refresh`
- **Expected**: Skill excluded from discovery and `read_skill` (error). Override file unchanged.
- **On failure**: Re-scan resurrects it.

#### [SK-07] enable override forces shadowed skill

- **Given**: Global authored `deploy`; project authored `deploy`; global override `enable`
- **Input**: `discover()`
- **Expected**: Global `deploy` forced into the union alongside project `deploy` (both listed, global tagged `force-enabled`). Override wins over precedence.
- **On failure**: Project shadows global despite override.

#### [SK-08] block records provenance

- **Given**: User runs `/set skills block some-skill`
- **Input**: Command
- **Expected**: Override `{state:"block"}` persisted. If the skill has author metadata, it is recorded in the override note. Skill excluded.
- **On failure**: Block is only session-scoped; forgotten on restart.

#### [SK-09] export graduates a learned skill

- **Given**: Learned skill `nightly-deploy` (score 0.8) in project store
- **Input**: `/set skills export nightly-deploy`
- **Expected**: `~/.config/amber/skills/nightly-deploy/SKILL.md` written from store content; `discover()` re-run. Learned entry still in store but suppressed by precedence. Score untouched.
- **On failure**: Export modifies store or fails silently.

#### [SK-10] export of unknown learned skill errors

- **Given**: No learned skill `ghost`
- **Input**: `/set skills export ghost`
- **Expected**: Error: `"no learned skill named ghost"`. Nothing written.
- **On failure**: Empty `SKILL.md` created.

#### [SK-11] /set skills show lists origins

- **Given**: Mixed catalog (project/global/interop/learned, one shadowed)
- **Input**: `/set skills show`
- **Expected**: Table per Scope mapping above, including `suppressed` for the shadowed learned skill.
- **On failure**: Origins omitted; user can't tell provenance.

#### [SK-12] /set skills create with --global

- **Given**: User types `/set skills create backup-script --global`
- **Input**: Enter; editor opens; save
- **Expected**: `~/.config/amber/skills/backup-script/SKILL.md` written; discover re-run; visible to all projects.
- **On failure**: Written to project scope by mistake.

#### [SK-13] /set skills refresh mid-session

- **Given**: Active session with stable discovery block; user adds a skill file on disk
- **Input**: `/set skills refresh`
- **Expected**: `discover()` re-run; next turn's prompt copy includes the new skill; KV-cache prefix invalidated from the discovery slot onward (documented cost, see `agent-skills.md`).
- **On failure**: Background auto-scan surprises the session.

#### [SK-14] read_skill oversized body rejected

- **Given**: Skill body 9,000 tokens; `skills_body_budget_tokens = 5000`
- **Input**: `read_skill("mega-doc")`
- **Expected**: `ToolResult{ok=false, error="skill body exceeds skills_body_budget_tokens"}`.
- **On failure**: Truncated body injected.

#### [SK-15] list_skills filtered

- **Given**: 7 skills; model calls `list_skills({origin:"authored"})`
- **Expected**: Returns only authored entries, same discovery format.
- **On failure**: Filter ignored; full dump.

#### [SK-16] Interop toggle + refresh

- **Given**: `skills_interop=false`; `.claude/skills/` populated
- **Input**: `/set skills interop on`, then `/set skills refresh`
- **Expected**: Interop skills appear in discovery and `show`. Config persisted.
- **On failure**: Interop skills missing until restart.

---

### Cross-references

- **Depends on**: `skills/agent-skills.md` (umbrella), `skills/skill-files.md` (format/discovery), `tui/input-system/nested-commands.md` (command tree model), `tui/input-system/slash-engine.md` (`/set` wiring), `config/file-config.md` (budget keys), `config/ui-config.md` (persistence), `workspace/security-model.md` (trust)
- **Depended on by**: `agent-loop/core-loop.md` (injection point), `prompts/skills.md` (discovery-block wording consumed from the catalog)
- **Test coverage**: `tests/skill_catalog_test.cpp` + `tests/run_tests.cpp` (see `docs/skills-tracker.md`)

### Known gaps

1. **No per-skill version pinning** — resolution is by name; reinstalls can change behavior under a stable name. `metadata.version` is stored but not enforced.
2. **No editor integration in CLI** — `create` opens the TUI editor (or a `$EDITOR` fallback in the CLI); a richer authoring UX is deferred.
3. **No marketplace / registry client** — skills are installed by the user into the scanned dirs; no `skills.sh` integration.
4. **No skill dependencies** — the standard's dependency model is not resolved; skills are standalone.

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Initial spec |

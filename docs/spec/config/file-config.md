## Spec: File Config (`amber.conf`)

### Purpose
Load key-value configuration from `amber.conf` in the project root. This is
a simple `KEY=VALUE` file that provides the baseline config for both CLI and
TUI, overrideable by CLI flags, env vars, and server auto-detection.

### Ownership
- **Source files**: `lib/config.cpp` (`Config::load()` — lines 12–70), `include/agent/config.h`
- **Test files**: `tests/run_tests.cpp` — config parse and validation tests (lines 34–260)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Text file, one `KEY=VALUE` per line. Lines starting with `#` or empty are skipped. |
| **Output** | `Config` fields populated from parsed keys. Unknown keys are silently ignored. |
| **Error states** | File not found → silent return (no error). Malformed line (no `=`) → skipped. |
| **Invariants** | See below. |
| **Thread safety** | Read once at startup, then immutable. |

### Invariants

1. File not found is not an error — the config simply remains at defaults.
2. Unknown keys are silently ignored — no error or warning.
3. Values may be optionally double-quote-wrapped; quotes are stripped.
4. `model` set to empty string means auto-detect (`model_explicit = false`).
5. `context_size` set to `0` means auto-detect (`context_explicit = false`).

---

### Scenarios

#### [FC-01] Basic key-value parsing

- **Given**: `amber.conf` exists with content
- **Input**: `api_base=http://localhost:8081/v1\nmodel=qwen\n`
- **Expected**: `cfg.api_base = "http://localhost:8081/v1"`, `cfg.model = "qwen"`, `cfg.model_explicit = true`.
- **On failure**: Values not parsed.

#### [FC-02] Empty model = auto-detect

- **Given**: `model=` in config
- **Input**: `model=\n`
- **Expected**: `cfg.model_explicit = false`. Server auto-detect fills model.
- **On failure**: Model set to empty string.

#### [FC-03] Comments and blank lines

- **Given**: Config with comments
- **Input**: `# This is a comment\n\napi_base=http://...\n`
- **Expected**: Comment lines and blank lines skipped. `api_base` parsed correctly.

#### [FC-04] Quoted values

- **Given**: Double-quoted value
- **Input**: `model="gpt-4"`
- **Expected**: `cfg.model = "gpt-4"`. Quotes stripped.
- **On failure**: Quotes remain in value.

#### [FC-05] Unknown key silently ignored

- **Given**: Unrecognised key
- **Input**: `foobar=xyz\n`
- **Expected**: Key ignored. No error. No config field set.
- **On failure**: Crash or warning.

#### [FC-06] Missing file

- **Given**: No `amber.conf`
- **Input**: `cfg.load("amber.conf")`
- **Expected**: `if (!in) return;` — silent return. Config stays at defaults.
- **On failure**: Crash or error message.

---

### Skills and experience keys

Keys governing the skills system (spec: `docs/spec/skills/`) and the learned
experience store (spec: `docs/spec/memory/`). `experience_*` keys already exist;
`skills_*` keys are new.

| Key | Default | Meaning |
|-----|---------|---------|
| `skills_max_discovery` | `20` | Max `name: description` entries in the injected discovery block (hard cap, `skills/agent-skills.md` [AS-06]) |
| `skills_body_budget_tokens` | `5000` | Max token budget for a `read_skill` body; oversized bodies rejected, not truncated ([AS-07]) |
| `skills_interop` | `false` | Scan `.claude/skills` and `.codex/skills` (opt-in, [AS-08]) |
| `experience_store_path` | *(empty)* | Absolute path override for the experience store. Empty = default `<workspace>/.amber/experience.json` (project-scoped). Legacy `~/.amber/memories.json` is migration-seeded once. |
| `experience_enabled` | `true` | Master switch for learned memory/skills |
| `experience_max_memories` | `20` | Top-K memories injected |
| `experience_max_skills` | `10` | Top-K learned skills injected (authored skills budget separately via `skills_max_discovery`) |

#### [FC-07] skills_interop defaults off

- **Given**: Config without `skills_interop`
- **Input**: `cfg.skills_interop`
- **Expected**: `false`. Interop scan disabled.
- **On failure**: Third-party skills loaded without consent.

#### [FC-08] skills budgets are parsed

- **Given**: `skills_max_discovery=15\nskills_body_budget_tokens=8000\n`
- **Input**: `Config::load()`
- **Expected**: Both fields populated. Out-of-range values rejected by `validate()` with a descriptive error.
- **On failure**: Budgets silently defaulted; caps not enforced.

#### [FC-09] experience_store_path override

- **Given**: `experience_store_path=/data/experience.json`
- **Input**: `load_experience_config(cfg)`
- **Expected**: Store loads/saves to the absolute path. Empty value → project default `<workspace>/.amber/experience.json`.
- **On failure**: Store resolves to a global path (cross-project leak).

#### [FC-10] Legacy store migration

- **Given**: `~/.amber/memories.json` exists; `<workspace>/.amber/experience.json` absent
- **Input**: First `Agent` construction in the project
- **Expected**: Project store seeded once from the legacy file; legacy file untouched; subsequent runs use the project store (see `skills/agent-skills.md` [AS-09]).
- **On failure**: Learned skills disappear after upgrade.

---

### Cross-references

- **Depends on**: `config/merge-semantics.md` (priority ordering)
- **Depended on by**: `config/cli-config.md`, `config/ui-config.md`, `skills/skill-files.md` (`skills_interop`), `skills/skill-catalog.md` (budgets)
- **Test coverage**: `tests/run_tests.cpp`: `config_defaults`, `config_validate_flags_problems`, `config_trailing_slash_validation`, etc. Skills keys: `tests/skill_file_test.cpp`, `tests/skill_catalog_test.cpp`.

### Known gaps

1. **Silent unknown keys** — No warning on typos (e.g., `apii_base` is silently ignored).
2. **No includes** — No `@include` directive for splitting config across files.
3. **No type validation at parse time** — String values accepted; type errors caught only at `validate()`.

## Spec: Config Merge Semantics

### Purpose
Define the priority ordering and field-level override rules when multiple config
sources (file, env, flags, auto-detect) contribute to the final `Config` state.

### Ownership
- **Source files**: `src/main.cpp` (CLI merge — lines 38–98), `tui/tui_main.cpp` (TUI merge — lines 39–77), `lib/config.cpp` (`load()`, `apply_environment()`, `save_global()`, `save_settings()`, `validate()`)
- **Test files**: `tests/run_tests.cpp` — config merge and intent preservation tests (lines 166–260)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Config file(s) + env vars + CLI flags + server auto-detect |
| **Output** | Single `Config` with deterministic field-level merge |
| **Error states** | Conflicting values → lower-priority source overwritten by higher. |
| **Invariants** | See below. |
| **Thread safety** | Startup only. |

### Invariants

1. Merge priority (highest to lowest): Env vars > `--config FILE` > CLI flags > `amber.conf` > `~/.config/amber/config` > defaults.
2. `model_explicit` and `context_explicit` flags prevent server auto-detect from overwriting user-specified values.
3. Provider fields (`api_base`, `api_key`, `model`, `context_size`) are stored in `~/.config/amber/config` only.
4. Non-provider fields are stored in `.amber/settings` only.
5. Server auto-detect only fills fields that are NOT marked explicit.

---

### Scenarios

#### [MS-01] Merge order — CLI flag overrides file

- **Given**: `amber.conf` has `model=qwen`, CLI flag `--model gpt-4`
- **Expected**: `cfg.model = "gpt-4"`, `cfg.model_explicit = true`.
- **On failure**: File value wins.

#### [MS-02] Merge order — env var overrides CLI flag

- **Given**: `AMBER_MODEL=claude`, CLI flag `--model gpt-4`
- **Expected**: `cfg.model = "claude"`.
- **On failure**: CLI flag wins.

#### [MS-03] Auto-detect only fills when not explicit

- **Given**: User set `model=gpt-4` explicitly, `context_size` left at 0
- **Input**: `apply_server_autodetect()` → server reports `model=llama`, `context_size=8192`
- **Expected**: `cfg.model = "gpt-4"` (unchanged), `cfg.context_size = 8192` (filled).
- **On failure**: User's explicit model overwritten.

#### [MS-04] Intent preservation on save

- **Given**: `model_explicit = false`, `context_explicit = false`
- **Input**: `cfg.save(path)`
- **Expected**: Saved file has `model=` (blank) and `context_size=0` — auto-detect will re-fire on next load.
- **Regression guard**: `config_save_preserves_autodetect_intent` test.

#### [MS-05] Provider vs project settings separation

- **Given**: `/set temperature 0.7` then `/model gpt-4`
- **Input**: Both commands
- **Expected**: `.amber/settings` contains `temperature=0.7`. `~/.config/amber/config` contains `model=gpt-4`. No cross-contamination.
- **On failure**: Provider fields leak into project settings or vice versa.

#### [MS-06] CLI loads amber.conf unconditionally; TUI conditionally

- **Given**: `amber.conf` exists, global config has `provider=openrouter`
- **CLI**: `amber.conf` loaded
- **TUI**: `amber.conf` ONLY loaded if global provider is "custom" or unset
- **On failure**: Inconsistent loading between CLI and TUI.

---

### Cross-references

- **Depends on**: `config/file-config.md`, `config/cli-config.md`, `config/ui-config.md`
- **Depended on by**: `llm-client/model-probe.md` (auto-detect)
- **Test coverage**: `tests/run_tests.cpp`: `config_save_preserves_autodetect_intent` (204), `config_blank_model_and_zero_context_stay_auto` (166), `autodetect_fills_only_auto_fields` (697)

### Known gaps

1. **CLI vs TUI merge asymmetry** — CLI loads `amber.conf` unconditionally; TUI loads it only as fallback. Different final config possible on same machine.
2. **No validation at merge time** — Invalid values from any source are only caught by `validate()` at the end.
3. **`save_settings()` omits `show_reasoning`** — Runtime-only toggle is not persisted across sessions.

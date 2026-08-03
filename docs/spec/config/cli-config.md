## Spec: CLI Config (Flags)

### Purpose
Process command-line flags for both the headless CLI (`amber-cli`) and the TUI
(`amber`). Flags override config file values and are themselves overridden
by environment variables.

### Ownership
- **Source files**: `src/main.cpp` (CLI flag parsing — lines 63–85), `tui/tui_main.cpp` (TUI flag parsing — lines 29–38), `lib/config.cpp` (flag application)
- **Test files**: No direct flag-parsing tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `argc`/`argv` |
| **Output** | `Config` fields set from flags |
| **Error states** | Unknown flag → usage message + exit. |
| **Invariants** | See below. |
| **Thread safety** | Startup only. |

### Invariants

1. `--model` sets `model_explicit = true`.
2. `--config FILE` loads an additional config file over the current state.
3. `--yes` / `--yolo` enables auto-approval (CLI only).
4. `--no-stream` disables SSE streaming (TUI only).
5. CLI flags override file config but are overridden by environment variables.

---

### Scenarios

#### [CC-01] CLI — `--model` sets explicit

- **Input**: `amber-cli --model gpt-4`
- **Expected**: `cfg.model = "gpt-4"`, `cfg.model_explicit = true`. Server auto-detect does not override.
- **On failure**: `model_explicit` remains false.

#### [CC-02] CLI — `--yes` auto-approves

- **Input**: `amber-cli --yes --prompt "run command"`
- **Expected**: `auto_approve = true`. Approval hook returns `AllowSession` for all gated tools.
- **On failure**: User prompted on non-TTY.

#### [CC-03] CLI — `--config` chain-loads

- **Input**: `amber-cli --config custom.conf`
- **Expected**: `cfg.load("custom.conf")` called after defaults and `amber.conf` but before env vars.
- **On failure**: File not found → silent (same as `load()` behaviour).

#### [CC-04] TUI — `--no-stream`

- **Input**: `amber --no-stream`
- **Expected**: `cfg.stream = false`. Agent uses buffered chat instead of SSE.
- **On failure**: Stream still enabled.

#### [CC-05] Unknown flag

- **Input**: `amber-cli --bogus`
- **Expected**: Usage message printed. Program exits.
- **On failure**: Crash or silent ignore.

#### [CC-06] `--help` / `--version`

- **Input**: `amber-cli --help` or `amber-cli --version`
- **Expected**: Prints usage/version string, exits.
- **On failure**: Continues execution.

---

### Cross-references

- **Depends on**: `config/merge-semantics.md`
- **Depended on by**: `config/ui-config.md`
- **Test coverage**: No direct tests.

### Known gaps

1. **`--no-stream` only in TUI** — CLI has no equivalent flag. Must use `AMBER_STREAM=0` env var.
2. **No `--temperature` or `--max-tokens` flags** — These can only be set via config file or env var.
3. **No `--verbose` or `--debug` flag** — Debug logging requires config file or env var.

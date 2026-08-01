## Spec: UI Config (Runtime Settings)

### Purpose
Provide runtime configuration changes via TUI commands (`/set`, `/model`,
`/provider`, `/settings`) and persist non-provider settings to `.amber/settings`
and provider settings to `~/.config/amber/config`.

### Ownership
- **Source files**: `tui/tui_input.cpp` (`cmd_set()`, `cmd_get()`, `cmd_model()`, `cmd_provider()`, `settings_screen()`, `save_settings()`), `lib/config.cpp` (`save_settings()`, `save_global()`)
- **Test files**: No direct tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Slash commands from TUI input |
| **Output** | Config mutations + persistence to disk |
| **Error states** | Invalid sub-key → status error. Missing value → `show_command_frame()`. |
| **Invariants** | See below. |
| **Thread safety** | Main thread only. Config changes apply before next agent `run()`. |

### Invariants

1. `/set` changes are saved to `.amber/settings` immediately.
2. `/model` changes are saved to `~/.config/amber/config` immediately.
3. `/provider` changes are saved to `~/.config/amber/config` and optionally to provider store.
4. Provider fields (api_base, api_key, model, context_size) are NEVER written to `.amber/settings`.
5. Detection/compression changes are propagated to all window agents via setter methods.

---

### Scenarios

#### [UC-01] `/set detection loop on` — persists

- **Given**: User types `/set detection loop on`
- **Input**: Enter
- **Expected**: `cfg_.detection_loop = true`. `cfg_.save_settings(settings_path_)` called. Settings file written. All window agents get `set_detection_loop(true)`.
- **On failure**: Setting not persisted across sessions.

#### [UC-02] `/model` — saves globally

- **Given**: User selects model from list
- **Input**: Enter on model name
- **Expected**: `cfg_.model = "selected-model"`, `cfg_.model_explicit = true`. `cfg_.save_global(global_path)` called.
- **On failure**: Model not persisted, or `model_explicit` false.

#### [UC-03] `/provider` — switches provider

- **Given**: User types `/provider openrouter`
- **Input**: Enter
- **Expected**: `cfg_.apply_provider("openrouter")`. Global config saved. Current model may be overwritten by provider default.
- **On failure**: Provider not applied.

#### [UC-04] `/settings` — full provider config

- **Given**: User opens settings screen, edits API base URL, tests connection
- **Input**: Multi-step dialog
- **Expected**: URL stripped of trailing slash. Connection test runs. On success, global config saved.
- **On failure**: Trailing slash remains.

#### [UC-05] `.amber/settings` — saved on auto-exit

- **Given**: User quits TUI
- **Input**: `/quit` or Ctrl+C
- **Expected**: `save_workspace_now()` persists all dirty windows. Settings are saved.
- **On failure**: Unsaved settings lost.

---

### Cross-references

- **Depends on**: `config/merge-semantics.md`, `config/file-config.md`, `tui/input-system/nested-commands.md`, `tui/settings-ui.md`
- **Depended on by**: `agent-loop/core-loop.md` (detection/compression settings)
- **Test coverage**: No direct tests.

### Known gaps

1. **`show_reasoning` not persisted** — Runtime-only toggle, forgotten on restart.
2. **No single "save everything" call** — Provider settings and project settings are saved separately.
3. **Running agents not notified of all changes** — Only detection/compression setters propagate. `model`/`api_base` changes only apply to new agents.

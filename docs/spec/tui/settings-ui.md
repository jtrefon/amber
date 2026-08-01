## Spec: TUI Settings UI

### Purpose
Provide interactive configuration of provider settings (API endpoint, key, model,
context size) via a multi-step modal dialog. Also handles `/set` runtime config
changes and persistence to global config file and provider store.

### Ownership
- **Source files**: `tui/tui_input.cpp` (`cmd_provider()`, `cmd_model()`, `settings_screen()` — lines 913–1091, `edit_provider_form()` — line 896)
- **Test files**: No direct tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `/settings`, `/provider`, `/model` commands, or `edit_provider_form()` dialog |
| **Output** | Updated `Config` fields, persisted to `~/.config/amber/config` and `~/.config/amber/providers/<name>.conf` |
| **Error states** | Connection test failure → status error. Invalid URL/key → form validation. |
| **Invariants** | See below. |
| **Thread safety** | Main thread only. |

### Invariants

1. `/settings` opens a multi-step provider configuration dialog.
2. The form strips trailing slashes from API base URLs.
3. Connection test runs a `/v1/models` probe and shows result.
4. Saved providers appear as presets in future sessions.
5. `/model <name>` fetches the model list from the server and validates the name.

---

### Scenarios

#### [SU-01] Settings screen — provider selection

- **Given**: User types `/settings`
- **Input**: Enter
- **Expected**: `ListPanel` shows: Built-in presets (openrouter, kilocode), saved custom providers, "Add new provider...". User selects one.
- **On failure**: Empty list or missing built-in presets.

#### [SU-02] Settings screen — activate & edit

- **Given**: User selects a provider
- **Input**: Select "Activate & edit"
- **Expected**: `edit_provider_form()` shows fields: Server URL, API Key, Model, Context Size. Current values pre-filled. Trailing slash stripped from URL.
- **On failure**: Missing fields or wrong defaults.

#### [SU-03] Settings screen — test connection

- **Given**: Provider configured
- **Input**: Select "Test connection"
- **Expected**: `test_connection()` → probes `/v1/models` → `info_dialog` shows success/failure. If success, detected model and context size shown.
- **On failure**: Hang on network timeout (no timeout parameter on test?).

#### [SU-04] `/model` — list and select

- **Given**: User types `/model`
- **Input**: Enter
- **Expected**: `cmd_model("")` → fetches model list from server via `agent::list_models()` → shows `list_panel`. User selects → model saved.
- **On failure**: Network error → status message.

#### [SU-05] `/model <name>` — set directly

- **Given**: User types `/model gpt-4`
- **Input**: Enter
- **Expected**: Sets `cfg_.model = "gpt-4"`, marks explicit. Saves to global config.
- **On failure**: Model not validated against server.

#### [SU-06] `/provider <name>` — switch provider

- **Given**: User types `/provider openrouter`
- **Input**: Enter
- **Expected**: `cmd_provider("openrouter")` → `cfg_.apply_provider("openrouter")` → sets `api_base`, `model`. Saves global config.
- **On failure**: Unknown provider name.

---

### Cross-references

- **Depends on**: `config/merge-semantics.md`, `tui/dialogs.md` (form_edit, list_panel, info_dialog)
- **Depended on by**: `config/ui-config.md`
- **Test coverage**: No direct tests.

### Known gaps

1. **No key-mask in form_edit** — API key is shown in plain text.
2. **Connection test may hang** — No timeout on the test request in some paths.
3. **No validation of model name against server** — `/model <name>` does not verify the model exists on the server.
4. **Settings not reflected in running agents** — Changing provider/api_key after agent started does not affect the running agent's LLMClient.

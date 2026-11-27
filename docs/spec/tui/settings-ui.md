## Spec: TUI Settings UI

### Purpose
Configure provider settings (API endpoint, key, model) through the documented
`/provider` verbs. `/settings provider` is a picker over the same verbs, never a
second action list. Also handles `/set` runtime config changes and persistence
to the global config and provider store.

### Ownership
- **Source files**: `tui/tui_input.cpp` (`cmd_provider*()`, `settings_screen()`,
  `provider_actions()`), `tui/provider_form.cpp` (`edit_provider_form()`),
  `completions.json` (the provider and settings nodes)
- **Test files**: `tests/tui_pty_test.cpp` (drawer, edit, inactive edit, key
  clear, delete confirmation), `tests/completions_test.cpp` (tree and picker
  contract), `tests/tui_agent_e2e_test.cpp` (management journey).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `/settings [group]`, `/provider add\|activate\|edit\|list\|test\|delete`, `/model` |
| **Output** | Updated `Config` fields, persisted to `~/.config/amber/config` and `~/.config/amber/providers/<name>.conf` |
| **Error states** | Unknown provider → status error. Rejected key → form reopens. Delete → confirmation. |
| **Invariants** | See below. |
| **Thread safety** | Main thread only. |

### Invariants

1. Every provider verb is a `completions.json` child; the picker renders
   `provider_verbs()` and dispatches the child’s action. No code-side verb list.
2. `/provider edit` with no name edits the active provider; editing an inactive
   provider writes its file but does not switch the session.
3. Editing the active provider updates the live agent, the global config and the
   provider file; a cleared key is cleared everywhere.
4. The form strips trailing slashes from API base URLs; a blank model stays
   “auto” instead of pinning the seeded value.
5. Connection test probes the selected provider, not the active connection.
6. `/provider delete` asks for confirmation and refuses the active provider.

---

### Scenarios

#### [SU-01] Settings screen — provider selection

- **Given**: User types `/settings provider`
- **Input**: Enter
- **Expected**: `ListPanel` shows built-in presets, saved providers and
  “+ Add new provider…”. Selecting a provider opens its documented verbs.
- **On failure**: Empty list, missing presets, or a verb not in the tree.

#### [SU-02] Provider edit

- **Given**: User selects a provider and “Edit a provider’s endpoint, key or model”
- **Input**: Change the Server URL and press Enter
- **Expected**: `edit_provider_form()` shows Server URL / API Key / Model.
  Saving an active provider lands the new endpoint in the live session and both
  files; saving an inactive provider leaves the active selection alone.
- **On failure**: Missing fields, wrong defaults, or a session switch.

#### [SU-03] Provider connection test

- **Given**: Provider configured
- **Input**: Select “Test the provider connection” or `/provider test <name>`
- **Expected**: Probes that provider’s `/v1/models` and reports the result.
- **On failure**: Testing the active endpoint instead of the selected one.

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
- **Test coverage**: `tests/tui_pty_test.cpp`, `tests/completions_test.cpp`,
  `tests/tui_agent_e2e_test.cpp`.

### Known gaps

1. **No key-mask in form_edit** — API key is shown in plain text.
2. **Connection test may hang** — No timeout on the test request in some paths.
3. **No validation of model name against server** — `/model <name>` does not verify the model exists on the server.

## Spec: Search Backend — Selection & Registration

### Purpose
Dispatch search queries to an enabled backend based on the `mode` argument.
Backends are plugin capabilities (`SearchBackendCapability`): the tool resolves
them through a provider over the runtime's owner-tagged registry, so adding a
backend is a plugin, never an edit to the tool.

### Ownership
- **Source files**: `tools/search_tool.cpp` (resolution and formatting),
  `lib/extensions.cpp` (`SearchBackendRegistry`, `make_search_backend_provider`,
  `SearchBackendCapability`), `plugins/search_grep/` and
  `plugins/search_semantic/` (the shipped backends), `lib/tools_default.cpp`
  (the bare-host path)
- **Test files**: `tests/search_backends_test.cpp` (registry, capability,
  runtime, and tool-level behavior), `tests/run_tests.cpp`
  (`search_tool_mode_switch` and the path/confinement tests)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `mode` argument from SearchTool (default `"grep"`) |
| **Output** | `SearchBackend` built fresh per resolution (the semantic backend caches its own index) |
| **Error states** | Disabled mode → error naming the plugin and `/set plugin <id> on`. Unknown mode → error with the enabled list. No backends enabled → "no search backend is enabled". Never a silent fallback. |
| **Invariants** | See below. |

### Invariants

1. The tool names no backend: every mode resolves through the
   `SearchBackendProvider` it was constructed with (`search_backend.h`).
2. A backend is created fresh on each resolution; any caching belongs to the
   backend instance (semantic caches per `(root, glob, excludes)`).
3. Disabled modes stay **known**: a plugin's declaration is recorded at
   registration (`declare`) and its owner-checked removal marks the mode
   unavailable, exactly as the dialect table treats provider flavors (D19).
4. Registry mutation is locked (toggled from the host thread, resolved from the
   agent thread); work already in flight keeps the backend it resolved.
5. The mode list rendered into the tool schema comes from the live registry and
   is rebuilt per request, so schema and registry cannot disagree.
6. One plugin per backend (`plugins/search_grep/`, `plugins/search_semantic/`),
   each independently switchable.
7. The bare-host path installs the same capability definitions directly
   (`register_default_tools`, `builtin_search_backend_provider`), so the shipped
   pair is one definition with two install paths.

---

### Scenarios

#### [BS-01] Mode dispatch — grep

- **Given**: an enabled backend for `"grep"`
- **Input**: `search("pattern", root, "", 200, "grep")`
- **Expected**: the grep backend runs. Results in `path:line:text` format.
- **Regression guard**: `search_tool_mode_switch`.

#### [BS-02] Mode dispatch — semantic

- **Given**: an enabled backend for `"semantic"`
- **Input**: `search("pattern", root, "", 200, "semantic")`
- **Expected**: the semantic backend runs. Results ranked with a `(score=X)`
  prefix.
- **Regression guard**: `search_tool_mode_switch`.

#### [BS-03] Disabled mode fails loudly

- **Given**: `search_semantic` switched off (`/set plugin search_semantic off`)
- **Input**: `mode="semantic"`
- **Expected**: an error naming `search_semantic` and
  `/set plugin search_semantic on`. **Not** a fallback to grep.
- **Regression guard**: `search_tool_disabled_mode_names_its_plugin`,
  `bundled_search_backend_plugins_provide_grep_and_semantic`.

#### [BS-04] Unknown mode lists what is enabled

- **Given**: `"grep"` enabled, `mode="nope"`
- **Input**: any query
- **Expected**: an error containing the unknown mode and the enabled list.
- **Regression guard**: `search_tool_unknown_mode_lists_enabled_modes`.

#### [BS-05] No backend enabled

- **Given**: every backend plugin switched off
- **Input**: any query
- **Expected**: "no search backend is enabled", with the requested mode named.
- **Regression guard**: `search_tool_with_no_backends_says_so`.

#### [BS-06] Tool registration

- **Given**: `register_default_tools(registry, jobs)`
- **Expected**: the tools are registered, and the search tool resolves the
  shipped backends without a runtime.
- **On failure**: a bare host's search tool cannot resolve any mode.

---

### Cross-references

- **Depends on**: `search-backends/grep-backend.md`,
  `search-backends/semantic-backend.md`, `plugins/plugin-framework.md` §4
- **Depended on by**: `tools/search-tool.md`
- **Test coverage**: `tests/search_backends_test.cpp`,
  `tests/run_tests.cpp`: `search_tool_mode_switch`

### Known gaps

1. **Ranked-output formatting is keyed to the backend name** — the tool emits
   `(score=…)` when `name() == "semantic"`. A third-party ranked backend would
   need either that name or a formatting-capability on the port.
2. **No health check** — Backends cannot report unavailability before search.

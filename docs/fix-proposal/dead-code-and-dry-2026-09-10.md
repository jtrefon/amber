# Dead Code & DRY — Proposal

## Problem

Three DRY/dead-code violations identified in the architectural review:

### 1. Dead `shell_split` function

`shell_split()` is defined in `tools/search/semantic_index.cpp` (line 86)
and declared in `include/agent/semantic_helpers.h` (line 33), but never
called anywhere in the codebase. It is pure dead code.

### 2. Search-mode string branching (OCP violation)

`tools/search_tool.cpp` lines 110-111 select the search backend via:

```cpp
if (mode == "semantic") backend = make_semantic_backend();
else backend = make_grep_backend();
```

And again at line 122 for output formatting. Adding a new backend
requires editing `SearchTool::execute()` — an OCP violation. The
`SearchBackend` interface already exists; the tool should look up
backends by name from a registry, not branch on strings.

### 3. Hardcoded choices in `build_settings()` (DRY violation)

`SlashDispatcher::build_settings()` in `tui/tui_input.cpp` hardcodes
choice lists like `{"on","off","toggle"}` and `{"read","write","yolo"}`
for each setting. These same choices already exist in
`completions.json` and are read by `SettingRegistry::choices_for()`.

The `Setting::choices` field populated by `build_settings()` is **never
read** — `drawer_rows.cpp` uses `choices_for()` (which reads from the
JSON tree), not `Setting::choices`. The field is dead data, and the
hardcoded lists duplicate the JSON tree.

## Target state

### 1. Remove `shell_split`

Delete the definition from `semantic_index.cpp` and the declaration from
`semantic_helpers.h`. No callers, no behavior change.

### 2. Search backend registry

Add a `SearchBackendRegistry` to `include/agent/search_backend.h`:

```cpp
class SearchBackendRegistry {
public:
    static SearchBackendRegistry& instance();
    void register_backend(const std::string& mode,
                           std::function<std::unique_ptr<SearchBackend>()>);
    std::unique_ptr<SearchBackend> create(const std::string& mode) const;
    std::vector<std::string> available() const;
private:
    std::map<std::string, std::function<std::unique_ptr<SearchBackend>()>> factories_;
};
```

Register `grep` and `semantic` at construction (or via a static
initializer). `SearchTool::execute()` looks up the mode in the registry
and creates the backend. Adding a new backend = register a factory, no
edit to `SearchTool`.

The output-formatting branch (`if (mode == "semantic")` at line 122) is
also eliminated: the backend's `name()` already identifies it, and the
output format difference (score vs. line) is handled by checking whether
`h.score > 0.0` (semantic backends set scores; grep uses match order as
score, which is always `>= 0` but the semantic format includes the score
in the output). Actually, the cleaner fix is to let the backend format
its own hits — add a `format_hit()` method to `SearchBackend`. But that
is a larger change. For now, use `backend->name()` instead of the `mode`
string for the output branch, eliminating the string comparison.

### 3. Remove dead `Setting::choices` field

- Remove the `choices` field from the `Setting` struct.
- Remove the `choices` parameter from the `add()` lambda in
  `build_settings()`.
- The `choices_for()` method (which reads from the JSON tree) remains
  the single source of truth for completion choices.

## Scope

- `include/agent/semantic_helpers.h`: remove `shell_split` declaration
- `tools/search/semantic_index.cpp`: remove `shell_split` definition
- `include/agent/search_backend.h`: add `SearchBackendRegistry`
- `tools/search_tool.cpp`: use registry instead of string branching
- `tools/search/grep_backend.cpp`: register `grep` backend
- `tools/search/semantic_backend.cpp`: register `semantic` backend
- `tui/setting_registry.h`: remove `choices` from `Setting` struct
- `tui/tui_input.cpp`: remove `choices` parameter from `build_settings()`

## Risk

Low. All changes are mechanical:
- Dead code removal has no behavior change.
- The registry is a lookup table; the existing factories are unchanged.
- The `choices` field is never read, so removing it changes nothing.

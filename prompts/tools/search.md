## search

Query the filesystem. Locates matches by regex (`mode="grep"`, the default) or
by meaning (`mode="semantic"`); `mode` selects an enabled search backend, and
the tool schema lists the backends enabled right now.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `pattern` | string | yes | — | Short regex or query (max 256 chars; longer patterns are rejected) |
| `path` | string | no | workspace root | Directory to search |
| `glob` | string | no | — | File filter, e.g. `"*.cpp"` or `"*.md"` |
| `mode` | string | no | `"grep"` | Search backend; the enabled set is listed in the tool schema |
| `max` | integer | no | 200 | Maximum matches to return |

Hidden dirs (`.git`, `.amber`) and vendored code (`third_party`) are skipped
by default. To search inside them, set `path` explicitly to a directory
within one — the default exclusion is dropped for that directory.

**Content**: matching lines with file paths and line numbers.
**Meta**: `{"hits": <count>, "mode": "<the backend used>"}`

## search

Query the filesystem. Locates matches by regex (`mode="grep"`, default) or
meaning-based ranking (`mode="semantic"`).

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `pattern` | string | yes | — | Short regex or query (max 256 chars; longer patterns are rejected) |
| `path` | string | no | workspace root | Directory to search |
| `glob` | string | no | — | File filter, e.g. `"*.cpp"` or `"*.md"` |
| `mode` | string | no | `"grep"` | `"grep"` for regex, `"semantic"` for meaning-based ranking |
| `max` | integer | no | 200 | Maximum matches to return |

Hidden dirs (`.git`, `.amber`) and vendored code (`third_party`) are skipped
by default. To search inside them, set `path` explicitly to a directory
within one — the default exclusion is dropped for that directory.

**Content**: matching lines with file paths and line numbers.
**Meta**: `{"hits": <count>, "mode": "<grep|semantic>"}`

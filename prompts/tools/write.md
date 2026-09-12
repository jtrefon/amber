## write

Edit a file using targeted replace blocks. Create new files by passing
an empty string as `old`.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `path` | string | yes | — | File to edit (confined to workspace) |
| `edits` | array | yes | — | List of `{"old": "...", "new": "..."}` blocks applied in order |

**Content**: count of edits applied.
**Meta**: `{"applied": <count>, "path": "<path>"}`

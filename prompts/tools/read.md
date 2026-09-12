## read

Read a file with pagination.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `path` | string | yes | — | File to read (confined to workspace) |
| `offset` | integer | no | 1 | Starting line number (1-based) |
| `limit` | integer | no | 200 | Maximum lines to return (hard max 2000) |

**Content**: lines prefixed with line number. Footer indicates more lines.
**Meta**: `{"lines": <returned>, "total": <file total>, "more": <true|false>}`

Binary files (NUL bytes, e.g. git objects or compiled artifacts) are refused —
read pages of text with `offset`/`limit` instead.

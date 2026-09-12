# Tools

## Working style

A few suggestions — adapt them to the task:

- Get oriented before searching: a quick `ls -loha` (and `tree -a -I
  'node_modules|.git|.amber'` when available) tells you what kind of project
  this is and what vendors it uses. The Environment section lists the OS,
  the user, and which tools are installed.
- `search` is good for quick symbol/string lookup; `bash` pipelines
  (`grep`, `find`, `sed`, `awk`) are just as valid when you need more
  control. Use whichever fits the task.
- Short commands belong in `bash`; long-running or streaming ones in
  `process_start`.
- Reads are paginated — request the ranges you need.

## Result envelope

Every tool result — regardless of which tool, regardless of success or
failure — uses the exact same form:

```
[tool=<name> status=<status> meta=<json>]
<content>
[end]
```

The `args` field echoes your original arguments when they are compact —
confirmation of what was sent. Large payloads (e.g. full file writes) are
not echoed.

The envelope is **immutable**. Only the values inside change. The shape
is always identical. This lets you parse any result the same way.

### Header fields

| Field | Always? | Meaning |
|-------|---------|---------|
| `name` | always | The tool that was called — matches your invocation |
| `status` | always | Outcome — one of the four values below |
| `meta` | always | Tool-specific metadata as JSON object (lines, exit code, hits, etc.). Empty `{}` if nothing to report. |

### Status values

| Status | Meaning | What to expect |
|--------|---------|----------------|
| `ok` | The tool completed successfully | Read the content for the result |
| `error` | The tool failed | The content explains the failure; the same arguments will fail the same way |
| `denied` | The tool was not approved (e.g. bash in READ mode) | Retrying will fail again — report to the user or use an alternative tool |
| `timeout` | The tool exceeded its time limit | The content may contain partial output; retry with adjusted parameters (timeout, scope) |

### Content section

- For **successful** calls: the result data (file contents, search hits,
  command output, status message)
- For **failed** calls: the error message (prefixed with "ERROR:")
- For **denied** calls: the denial reason

The content is always followed by `[end]` on its own line.

---

## Tool categories

| Category | Tools | Behaviour |
|----------|-------|-----------|
| **Query** | `search`, `read` | Read-only. Return data in content. Safe, no side effects. |
| **Command** | `write`, `bash`, `process_*` | Side effects. Return status summary in content. May require approval. |

---

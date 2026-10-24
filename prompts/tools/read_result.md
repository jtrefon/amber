## read_result

Read part of a tool output that was larger than the context budget.

When a tool result is truncated, its marker carries a handle:

```
[... 74 KiB omitted of 106 KiB total; full output: read_result result_id=9f3c1a7e52b84d10]
```

Call `read_result` with that handle to page through the original, byte by byte:

- `result_id` — the handle from the truncated result.
- `offset` — byte offset to start at (default 0).
- `limit` — maximum bytes to return (default 16384, up to 65536).
- `grep` — a substring; returns matching lines with their line numbers instead
  of a byte range, which is the quickest way to find one detail in a large log.

The output belongs to this session. Artifacts are kept under the project's state
directory and aged out oldest-first, so an old handle can report `unknown or
expired artifact`; the original tool call can be repeated when that happens.

## Spec: read_result Tool

### Purpose

Give the model a way back to a tool result that was too large for the context
budget. The context holds a bounded head/tail plus a handle; the full payload
lives in the project's artifact store.

### Ownership

Core artifact store (`lib/artifacts.cpp`), tool definition (`tools/read_result_tool.cpp`,
installed by the `tool_read` plugin), and the envelope marker
(`lib/agent_helpers.cpp`).

### Contract

| | |
|---|---|
| **Name** | `read_result` |
| **Role** | Read (read-only, no approval) |
| **Input** | JSON object: `{"result_id": <string>, "offset": <optional int>, "limit": <optional int>, "grep": <optional string>}` |
| **Output** | A byte slice of the stored payload, or (with `grep`) matching lines prefixed `N:` |
| **Meta** | `{"result_id": <id>, "offset": <bytes>, "bytes": <returned>}` |
| **Bounds** | `offset` clamps to 0; `limit` defaults to 16384, max 65536; `grep` output shares the limit |
| **Retention** | 64 artifacts / 32 MiB per project, oldest-first eviction |

### Invariants

1. The id is the only path input; it must be 16 hex characters, so nothing a
   caller supplies can traverse out of the artifact directory.
2. An unknown or evicted id returns `ok=false` with `unknown or expired artifact <id>`;
   it never silently returns an empty payload.
3. A stored artifact is only created for payloads above `max_tool_result_bytes`
   and at least 4 KiB; smaller results stay inline.
4. Files are created owner-read/write only.
5. `grep` is a literal substring match; a single huge line is windowed around the
   match instead of being returned whole.

### Scenarios

#### [RR-01] Page through a stored result

- **Given**: A tool result of 100 KiB that was paged out with id `9f3c1a7e52b84d10`
- **Input**: `{"result_id": "9f3c1a7e52b84d10", "offset": 0, "limit": 8}`
- **Expected**: The first 8 bytes of the original payload; meta reports `bytes: 8`
- **Regression guard**: `read_result_tool_reads_a_stored_slice`

#### [RR-02] Find a line in a large log

- **Given**: An artifact whose line 42 contains `needle`
- **Input**: `{"result_id": "<id>", "grep": "needle"}`
- **Expected**: `42:...needle...`; lines that do not match are omitted
- **Regression guard**: `artifacts_grep_returns_numbered_matches`

#### [RR-03] Invalid and expired handles are errors

- **Given**: The id `../../etc/passwd` and an id that was evicted
- **Input**: `{"result_id": "../../etc/passwd"}` and `{"result_id": "<evicted>"}`
- **Expected**: `ok=false`; `invalid artifact id` and `unknown or expired artifact <id>`
- **Regression guard**: `artifacts_reject_invalid_and_expired_ids`

#### [RR-04] Retention evicts the oldest

- **Given**: More than 64 stored artifacts
- **Input**: Another store
- **Expected**: The oldest file is removed, the newest is readable, and the directory never exceeds the cap
- **Regression guard**: `artifacts_retention_evicts_the_oldest`

#### [RR-05] The truncated envelope names the handle

- **Given**: A tool result above the budget with `meta.result_id`
- **Input**: `format_tool_envelope`
- **Expected**: The middle marker reads `[... N bytes omitted of M total; full output: read_result result_id=<id>]`
- **Regression guard**: `tool_envelope_marker_names_the_result_handle`

#### [RR-06] A plain message still digests

- **Given**: A bulky tool message with no envelope (recorded before the budget)
- **Input**: `tool_result_digest`
- **Expected**: `tool=<name> status=unknown bytes=<size>`, no crash on a missing header
- **Regression guard**: `tool_result_digest_falls_back_for_a_plain_message`

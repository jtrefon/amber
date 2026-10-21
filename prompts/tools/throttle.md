## throttle

Inspect or cap the active provider's request pacing. The governor is shared by
every request to that provider (chat, compression and subagents), so a cap is a
provider-wide ceiling, not a per-turn one.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | yes | `status`, `set` or `reset` |
| `interval_ms` | integer | no | Minimum spacing, 0–60000 (0 = no manual floor) |
| `concurrency` | integer | no | In-flight ceiling, 0–8 (0 = no manual cap) |
| `token_cap` | integer | no | Tokens per 60 s, 0 = no manual cap |
| `ttl_seconds` | number | no | How long the manual cap lives, 1–3600 (default 300) |

`status` prints the current interval, pause, concurrency, token/request budget and
consecutive rate limits. `reset` clears the manual cap immediately. The automatic
governor still adapts underneath a manual cap and never exceeds it while it lives.

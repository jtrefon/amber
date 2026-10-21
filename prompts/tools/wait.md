## wait

Pause this turn for a bounded number of seconds. Use it when a provider rate-limit
notice asks for a longer window than the automatic retry already waited, or before
a deliberate retry. The wait is cancellable.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `seconds` | number | yes | 0–300, cancellable |
| `reason` | string | no | Why the pause is needed; echoed back |

The governor also waits automatically after a 429. This tool is for a one-off pause
the model chooses, not a replacement for the governor's pacing.

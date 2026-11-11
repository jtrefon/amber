# Spec: prompt-cache reuse gauge

**Status:** 1.0 plugin design. **Owner:** `plugins/cache/` + the TUI status bar.

## Contract

The gauge answers **"what is the provider reusing now?"**, not "what did this
session average over its lifetime?". Prompt caches expire on a provider TTL
(5m/1h for Anthropic; implicit caches elsewhere), so a session that spans days
is a sequence of independent cache epochs.

The window is:

- the last **8** supported main-conversation calls (`samples` setting: 4/8/16);
- no older than the provider cache TTL (`prompt_cache_ttl`, 5m/1h; `off` hides
  the gauge);
- reset on provider or model change, on plugin disable, and on
  `/plugin cache reset`;
- never a lifetime total, and never persisted into a restored session.

A sample is one completed `LlmResponseEvent` with `prompt_tokens > 0` and
`cache_read_tokens >= 0`. A sample with no cache counters marks the window
unsupported. Calls older than the TTL make the state `Cold`.

## Numbers

For the samples in the window:

| Readout | Formula |
|---|---|
| **Token reuse** (default headline) | `sum(cache_read_tokens) / sum(prompt_tokens)` |
| **Call hit rate** (Hermes-style, `metric=calls`) | `calls with cache_read > 0 / calls` |
| `call_hit_percent` | always `calls with cache_read > 0 / calls` |
| `input/read/wrote` | raw token sums in the window |

Rounding is **floor**, never round-up: 99.7 shows `99%`, 100 only when every
token in the window was cached. `0 < ratio < 1%` shows `<1%`. No decimals.

## States

| State | Bar |
|---|---|
| no completed supported request | no segment |
| provider reports no counters | no segment; `/plugin cache` explains |
| cache off / TTL off | no segment |
| newest sample older than TTL | `cache cold` |
| only writes, no reads in window | `cache warm` |
| at least one read | `cache N%` |
| measured zero | `cache 0%` |

`cache warm` distinguishes a cache-creating first call from a 0% miss.
`cache cold` is the honest answer after idling past the TTL.

Tone: `Good` at >=70%, `Dim` at 1–69%, `Warn` at 0%; `cold`/`warm` are `Dim`.
Text carries the meaning; colour is supplementary and works in ASCII/monochrome.

## Placement

- `StatusSegmentCapability("cache", priority=650, drop_priority=6,
  StatusAlign::Left, StatusZone::Bottom)`.
- Between `tps` (600) and the token totals (700); drops before the token totals
  on a narrow bar.
- `cache 87%` / `cache cold` / `cache warm`; no sparkline.

## Surfaces

- Status segment: one number.
- `/plugin cache`: per-session detail:
  - headline metric, call hit count, input/read/write tokens, model/provider,
    last request age, last miss reason;
  - one block per tracked session;
  - `reset` clears all windows.
- Settings: `metric=tokens|calls`, `samples=4|8|16`.

## Core seam

The plugin cannot read the agent's private stats, so the core publishes the
typed observation it already records:

- `LlmResponseEvent` carries `cache_read_tokens`, `cache_write_tokens`,
  `cache_miss_reason`, `owner_id`, `model`, `provider`;
- `StatusSnapshot` carries the active `owner_id`;
- no new event type, no per-token event, no lifetime accumulator.

## Non-goals (v1)

- Dollar estimates or a price table. Cache writes have a different price from
  reads; token counts are the honest unit until pricing exists.
- Session-wide averages, history graphs, or persisted gauge state.
- Subagent/compression/watch samples; only main-conversation calls are in the
  window.
- A panel; the command carries the detail.
- External-process plugin tier (it is tools-only).

## Acceptance

- Eight cached calls -> high reuse; one miss changes the window, not a lifetime
  average.
- After TTL idle -> `cache cold`, not the last percentage.
- Write-only first call -> `cache warm`, not `0%`.
- No counters -> no bar space used.
- Provider/model switch resets.
- Two windows show different values.
- `metric=calls` matches the detail's call hit count.
- 99.7 -> 99, 0.4 -> `<1%`, exact 100 -> 100.
- `make test` and `make check` green.

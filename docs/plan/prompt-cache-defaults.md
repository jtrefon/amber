# Proposal: prompt caching on by default, with optimal parameters

Status: **proposal, awaiting sign-off.**
Supersedes the opt-in surface in #521 (`AMBER_CACHE_SCOPE` / `AMBER_CACHE_DIAG`
as enablers). #521's mechanics stay; its defaults and switch direction change.

## Why this exists

Amber has shipped three cache fixes (#492, #497, #509) and a real account still
reports zero cached tokens, while **Claude Code through the same gateway caches
normally** (Opus ~10x cheaper than amber on Sonnet). That single comparison rules
out the gateway, the workspace theory and the "unauthorised client" theory: the
difference is the request.

The failure has been diagnosable but not attributable. Caching was also
**opt-in-ish**: `off` was the only first-class switch, `scope` and diagnostics
were env-gated enablers, and the default request shape was a guess. A feature
that dominates cost should be on by default, with the best-known parameters, and
with a way to say *why* it missed.

## Goals

1. Caching is **on by default**, at parameters measured to be optimal, for every
   provider that supports it.
2. Configuration can only **narrow** the default (disable / downgrade), never be
   required to turn it on.
3. A cache miss is **attributable** to a tier, without a second experiment.
4. The prefix contract is enforced by tests, including mutation tests of the
   tests themselves.

Non-goals: a cache for the local llama endpoint (it has its own KV cache), any
client-side cache of prompts or responses, and any change to how `Context`
stores messages.

## Decision 1 — cache is on by default, per provider

| Provider | Mechanism | Default |
|---|---|---|
| Anthropic Messages | explicit `cache_control` breakpoints | **on** |
| OpenAI-compatible | automatic prefix caching | **on** (nothing to send; plus `prompt_cache_key`) |
| Gemini | implicit caching | **on** (nothing to send) |

`prompt_cache_ttl=off` remains the one-off disable; `AMBER_CACHE=off` is the
per-process disable. There is no switch that must be set to enable caching.

## Decision 2 — the optimal request shape

### Anthropic

| Parameter | Default | Why |
|---|---|---|
| Breakpoints | **2, explicit** | one on the stable prefix (tools + `system[0]`), one on the last real conversation turn. Explicit is the only way to place them; automatic places one on the volatile tail, which is the bug #509 fixed. |
| Top-level `cache_control` | **not sent** | It is the *alternative* mode, not a documented combination. It cannot carry `scope`, and it consumes one of the 4 slots. #497 added it for a dashboard; the protocol does not need it. |
| `scope` | **`"global"`** | Claude Code's shape, and the one that survives a gateway that does not pin every request to one upstream identity. Requires `anthropic-beta: prompt-caching-scope-2026-01-05`. |
| TTL | **`5m`** | Anthropic's own guidance: a 5-minute entry refreshed on every hit costs nothing extra, while `1h` bills **2x** on every write. At 180k tokens a wrong `1h` is the difference between ~$0.53 and ~$0.85 per miss. `1h` is offered, not defaulted. |
| Diagnostics | **off** | The production account is the only Anthropic endpoint available, and standing debug on in production is not acceptable: it is a per-request field whose only value is post-mortem. It stays as a debug-only switch (`AMBER_CACHE_DIAG=1`), default off. The cost signal (`read`/`write` per request) is the production observable. |
| Minimum prefix | **checked, and reported** | Below the model minimum nothing caches and no error is returned. `/get provider` already names the threshold; the client should also log when the marked prefix is below it. |

### OpenAI-compatible

Automatic caching needs no marker, but hit rate depends on **routing** and on an
identical prefix. Send `prompt_cache_key` = a stable hash of the rendered
`tools` + `system` (not of the conversation), so the provider can route
requests that share a prefix to the same cache. This is additive and ignored by
providers that do not implement it.

### Gemini

Implicit. Nothing to send. Prefix stability is the whole contract.

## Decision 3 — the configuration surface narrows only

Precedence: CLI flag > environment > project config > global config > default.

| Setting | Default | Narrowing values |
|---|---|---|
| cache | on | `off` |
| TTL | `5m` | `1h`, `off` |
| scope | `global` (Anthropic) | `session` |
| diagnostics | on (Anthropic) | `off` |

Environment variables are **disablers only**:

- `AMBER_CACHE=off`
- `AMBER_CACHE_SCOPE=session`
- `AMBER_CACHE_DIAG=0`

The enabling spellings (`AMBER_CACHE_SCOPE=global`, `AMBER_CACHE_DIAG=1`) are
removed before release; accepting them would keep the "must opt in" shape.

Command surface (all under the existing `/set provider cache` namespace):

- `/set provider cache 5m|1h|off`
- `/set provider cache scope global|session`
- `/set provider cache diag on|off`

The command tree is JSON, so these are three children, not three new handlers.

## Decision 4 — degrade, don't fail, and remember

A proxy that rejects `scope` (Portkey returns 400 on it) or `diagnostics` must
not take the request down. One retry policy, applied once per feature per
process:

1. A 400 whose body names `cache_control`, `scope`, `ttl` or `diagnostics`
   identifies the offending feature.
2. Retry once without that field.
3. If the retry succeeds, disable the feature for the session, record a
   `cache_degraded` event, and show it in `/get provider`.
4. Never retry twice; never retry a request that failed for another reason.

This is what makes default-on safe: the default is optimal, and an
incompatible gateway costs one extra request once, not a dead session.

## Decision 5 — observability that answers "why"

Per request, record and surface:

- `prompt`, `cache_read`, `cache_write` for **that** request (not a lifetime sum
  — the current `/get provider` sums the whole session, and `session_usage.h`
  restores those counters on load, which is why amber said "reading from cache"
  while the dashboard said zero).
- `cache_miss_reason` when the provider reports one.
- A hit-rate line: `cache: 12 req · 9 hit · 3 miss (system 2, tools 1)`.

The session log carries the same record, so a report can be attached to a bug
without a second run.

## Decision 6 — the test strategy (the part that was missing)

Three layers, each of which must be able to fail:

1. **Unit — body shape.** Every provider × setting: breakpoint placement, no
   top-level field, `scope` + beta together or neither, `off` sends nothing,
   `previous_message_id` is `null` on the first turn and replayed after.
2. **Integration — prefix invariant.** A long (50+ turn), multi-tool session
   driving the real `Agent` through the real dialect into a recording client,
   for each provider. At every request assert:
   - tier 1 (tools + stable system) is byte-identical;
   - the conversation is append-only up to the marked block;
   - the marked block is the last *real* turn, never a harness tail.
3. **Permutations.** Brief refresh, memory write, skill activation, compression,
   tool add/remove, plugin enable/disable, model switch, cancel mid-turn. Each
   asserts *where* a divergence is allowed (tail only) and that nothing else
   moved.
4. **Mutation tests of the harness.** Deliberately inject a timestamp into the
   system prompt, reverse tool order, drop `cache_control`, move the breakpoint
   onto the tail, rewrite one historical block, shift `scope` off the beta.
   The harness must go red for each. A harness that cannot detect these is how
   three fixes shipped green.

CI runs 1–4 hermetically. A live pre-release check (`/cache check`, two small
requests against the configured provider) is documented and manual, because CI
has no live model and a hermetic green run is not evidence about a provider.

## Rollout

- **0.8.6**: default-on Anthropic shape (explicit + `scope` + diagnostics, no
  top-level field), degrade-and-remember, the observability line, and the
  harness layers 1–3. This is the release that should make the production
  account cache.
- **0.8.7**: `prompt_cache_key` for OpenAI-compatible providers, mutation
  layer 4, `/cache check`.
- Docs: `docs/spec/llm-client/prompt-caching.md` is rewritten around the
  default-on shape; `AGENTS.md`'s cache section points at it.

## Risks

| Risk | Mitigation |
|---|---|
| A gateway 400s on `scope` | degrade-and-remember (Decision 4) |
| `scope: global` has a privacy reading | it is provider-sanctioned and used by Claude Code; a hit still requires byte-identical tokens. `session` is one setting away. |
| `1h` quietly doubles write cost | not a default; the docs state the 2x explicitly |
| Diagnostics adds a field a proxy rejects | same degradation path; it does not affect the cached prefix |
| The harness passes while production misses | layer 4 (mutation) plus the live check; the harness is tested for its ability to fail |

## Decisions (signed off)

1. **`scope: global` is the default** for Anthropic. It is Claude Code's shape,
   it is the only variable that differs from the one client that caches through
   this gateway, and degrade-and-remember covers a proxy that rejects it.
2. **Diagnostics default off.** The production account is the only reachable
   Anthropic endpoint, and standing debug on in production is not acceptable.
   It remains available as `AMBER_CACHE_DIAG=1` for a targeted investigation.
3. **The top-level automatic `cache_control` is removed entirely.** It is the
   alternative mode, cannot carry `scope`, and costs a breakpoint slot.
4. **TTL stays fixed at `5m`.** Adaptive TTL is a second variable on the one
   path we cannot test locally; it waits until a cache hit has been observed.
5. **`prompt_cache_key` (OpenAI-compatible) lands in 0.8.7**, not 0.8.6. It is
   the only change that touches a provider we cannot verify against, and 0.8.6
   should change one wire protocol at a time.

## What this changes in #521

- `AMBER_CACHE_SCOPE=global` disappears: `scope: global` is the default, and
  `AMBER_CACHE_SCOPE=session` is the only spelling that remains.
- `AMBER_CACHE_DIAG` stays, but as a debug-only enabler (default off).
- The top-level automatic field is removed from the body entirely.
- `AMBER_CACHE=off` is added as the one-switch disable.
- Degrade-and-remember, the per-request readout, and the prefix harness are
  added; #521 shipped none of those.

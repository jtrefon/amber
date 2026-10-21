## Spec: Provider rate governor

### Purpose

Every LLM HTTP request — chat, compression, subagents, every window — passes one
provider-scoped protection proxy. The governor decides *when* a request may
start and learns how much latency the provider needs, so a 429 is a pause rather
than a dead turn and a burst never becomes a retry storm.

This replaces the old "stop on the first 429" policy. That policy avoided a
flood by killing the work; this one avoids it by pacing.

### Ownership

- **Source**: `include/agent/rate_governor.h`, `lib/rate_governor.cpp`.
- **Wire point**: `lib/http_transport.cpp::curl_exec` acquires before the
  request and reports the status/headers/usage after it.
- **Retry policy**: `lib/agent_helpers.cpp` (`chat_with_retry`) retries a 429;
  the governor, not a sleep in the retry loop, owns the wait.
- **Tools**: `plugins/tool_rate/` (`wait`, `throttle`).
- **Tests**: `tests/rate_governor_test.cpp`, the retry/429 tests in
  `tests/agent_loop_test.cpp`.

### Control law

All methods take the current `steady_clock::time_point`; tests supply a fake
clock.

| State | Cold start | Rule |
|---|---|---|
| interval | 0 | Minimum spacing. Raised only after **sustained** rejection; a success halves it, two successes clear it. |
| concurrency | 4 | In-flight limit. Grows by one per success (max 8); halves on a repeated 429 (min 1). |
| pause | none | A provider deadline (`Retry-After` or the exponential fallback). Shared by every caller; one-off. |
| token/request budget | unknown | Enforced only when the provider publishes limits (`x-ratelimit-*`, `anthropic-ratelimit-*`). A reset refills the window. |

Admission waits only for the violated axis:

```
wait = max(pause_until - now,
           token_deficit / refill,
           last_start + interval,
           concurrency_slot)
```

- **First 429**: set `pause_until = now + Retry-After (+ jitter)`; do **not**
  raise the interval or lower concurrency. The next request is a single probe
  after the pause. This is the 4 s case: one 4 s pause, not a 4 s interval for
  the next minute.
- **Repeated 429** (second consecutive rejection): AIMD — double the interval
  (starting at 250 ms, cap 8 s), halve concurrency, reduce the token budget.
  This is the "we keep being rejected" case.
- **Success**: fast recovery — halve the interval, clear it after two successes,
  increment concurrency, grow the token budget or use the provider's remaining
  headers.
- **Idle 60 s**: clear the learned interval/concurrency penalty and let the next
  request go at full speed. A server pause is never discarded early.
- **No Retry-After**: exponential pause (1/2/4/8 s, cap 60 s) with jitter; the
  second consecutive rejection starts the AIMD interval.
- **5xx/transport**: in-flight released, no pacing penalty; the ordinary retry
  ladder decides.

### Provider isolation

One governor per `provider_name + api_base + hash(api_key)`. A 429 at one
provider never slows another. Within one account the quota is genuinely shared,
so the pause is shared — but it is one-off and decays.

### Tools

- `wait(seconds, reason?)` — one-off, bounded 0–300 s, cancellable.
- `throttle(action, interval_ms?, concurrency?, token_cap?, ttl_seconds?)` —
  `status` reports the live state; `set` applies a bounded ceiling (default TTL
  300 s); `reset` clears it. The manual ceiling applies to the provider, never
  exceeds the automatic policy, and expires.
- Manual ceilings are a **ceiling**, not a replacement: the automatic governor
  can still recover underneath them.

### Invariants

1. The first request after a cold start is admitted immediately: no artificial
   startup interval.
2. A 429 never ends the turn, the session, or the conversation. It is a pause.
3. The first 429 never raises the sustained interval; only repeated rejection
   does.
4. A retry is a single probe after the pause. No parallel retry burst.
5. A success is evidence of headroom and decays the penalty quickly.
6. A server pause is authoritative; an idle reset never shortens it.
7. A 429 at one provider cannot delay another provider.
8. Token/request budgets are enforced only from published headers, never from a
   guess.
9. The governor is the single pacing authority; the retry loop never sleeps on
   its own for a rate limit.

### Scenarios

- **Cold start**: first request has zero wait.
- **4 s rejection**: one 4 s pause, retry; a success returns to full speed.
- **Sustained rejection**: interval 250 → 500 → …, concurrency halves, then a
  success streak clears it.
- **260k × 4 turns**: the transport estimates each request's token cost; with
  published limits it reserves before sending; without them the pause/AIMD
  handles the first rejection and the next turn is not stopped.
- **Idle recovery**: after a quiet minute the next request goes at full speed.
- **Provider isolation**: an Anthropic 429 leaves an OpenAI request untouched.
- **Retry storm**: one probe per pause, timestamps prove the spacing.

### Revision history

| Date | Reason |
|------|--------|
| 2026-10-21 | Initial governor: cold start, pause vs interval, AIMD, token/request headers, fast recovery, provider isolation, wait/throttle tools. |

# Plan: let the catalogue cache re-resolve the context window

- **Status:** proposal for sign-off. Red tests are committed; no production
  code has been written yet.
- **Symptom:** with the local `custom` provider defaulting to 262,144 and
  `teacher-q8` advertising 160,000, the gauge showed 262k and only a manual
  `/set model context auto` plus a restart fixed it.
- **Issues:** no filed issue yet; this is a regression from the derived-pin
  paths in `http_transport.cpp`, `providers_service.cpp` and the regen gate in
  `Agent::set_context_window()`.

## Why the cache regen did not fix it

The regen exists and is fully wired:

```
model_catalog_refresh_async()  ->  force GET /v1/models, rewrite the cache
on_models_refreshed()          ->  apply_cached_server_autodetect()
apply_detected_context_window()->  Agent::set_context_window()
Agent::set_model()             ->  re-resolve from the picked model's caps
```

Every one of those ends at `if (cfg_.context_explicit) return;` or an
equivalent guard. A stale pin makes the correct 160,000 arrive twice and be
discarded twice. Two code paths create pins that are not user decisions:

- `lib/http_transport.cpp:232,262` — an HTTP 400 overflow sets
  `context_explicit = true` on the client config. `save_global()` later writes
  `context_size=` and the runtime fact becomes a permanent override.
- `lib/providers_service.cpp:241` — `provider_edit_seed()` marks the context
  explicit whenever the provider file has a `default_context_size`, contradicting
  the documented rule at `lib/config.cpp:206-209` that a provider default is an
  auto value.

A third gap: `clear_model_context()` drops the pin but never re-probes, so the
readout stays `auto (unknown)` until the next process starts.

## Target state

1. **Only a user pin is explicit.** `/set model context <n>` (and
   `AMBER_CONTEXT`) remain the only sources of `context_explicit = true`. They
   keep their current authority and the #672 mismatch warning stays.
2. **Runtime learning stays runtime.** The 400 overflow still teaches the
   window through `learned_context_size()` / `LearnedWindowCapture`, which
   `Agent::resolve_window()` already uses to clamp below the configured
   window. Drop the `context_explicit = true` writes in `http_transport.cpp`.
3. **Provider defaults stay auto.** Remove the seed's explicit flag; the
   default is shown in the form and applied by `apply_selection()`, then
   replaced by the listing when the catalog resolves.
4. **Clearing a pin re-resolves live.** `clear_model_context()` re-probes the
   cached catalogue and pushes the advertised window into the idle agents, so
   no restart is needed. `context_window_label()` falls back to the advertised
   window instead of printing `auto (unknown)`.
5. **The gauge shows the contradiction.** When a pin disagrees with the
   listing by more than 10%, the readout carries both numbers, e.g.
   `12k/256k (adv 1m)`, so the warning is not only a status line. The pin still
   governs the compression budget.

Legacy configs that contain `context_size=` without a provenance marker are
treated as genuine user pins — clearing one stays an explicit, deliberate act.
The fix prevents new derived pins; it does not silently rewrite old ones.

## Red tests (already committed at `1395a8c4`)

- `overflow_400_learns_the_window_without_a_pin` — a 400 teaches 16,384 for
  the client but leaves `context_explicit` false, so `save_global()` cannot
  freeze it. **Fails today** at `ASSERT_FALSE(cfg.context_explicit)`.
- `provider_edit_seed_never_turns_a_provider_default_into_a_pin` — a provider
  default of 262,144 is shown but not marked explicit. **Fails today.**
- `context_auto_regen_reloads_the_advertised_window` — after clearing a pin,
  `/set model context auto` prints `auto (1048576 tokens)` in the same session.
  **Fails today** (`auto (unknown)` until restart).

A fourth test for item 5 (gauge shows both numbers) will be added with the
production change once the readout format above is confirmed.

## Verification to run after green

- `./run_tests` (full suite), the PTY suite, `make tui-agent-e2e`,
  `make verify-quick`, `make lint-changed BASE=origin/main`.
- Manual: on a fresh config with the `custom` provider default of 262,144 and
  `teacher-q8` selected, the startup line reports 160,000 and the gauge shows
  160k without any `/set model context` command; forcing a catalogue refresh
  keeps it at 160k.

## Scope boundary

No change to the pin/listing authority rule from #672, no new config file key,
no change to the compression budget calculation beyond the values it receives.
The DeepSeek `completion_tokens_details.reasoning_tokens` work (#618) is a
separate issue and stays out of this change.

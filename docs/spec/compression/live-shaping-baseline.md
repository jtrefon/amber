# Live shaping baseline

The hermetic suite proves the harness. It cannot prove what a model does with
the harness: whether it keeps a fact across a compaction, whether it trims
evidence that has stopped mattering, and what the answer costs in tokens. This
file records the live measurement, the command that produces it, and what the
endpoint can and cannot report.

## The suite

`bench/scenarios/shaping/` holds live-only scenarios (no `fake_replies`, so a
hermetic run skips them and CI stays hermetic):

| Scenario | What it measures |
|---|---|
| `s-01-stale-early-evidence` | a deployment token read once at the start survives a five-file survey; the early read is what shaping may digest, the token is what must come back |
| `s-02-superseded-read` | the same file read before and after a write; a superseded-read trim must not leave the answer stale |

Both carry a retention oracle (`checks.must_contain`), an oracle of the tool
steps, and a step/time budget. A scenario with no retention oracle fails
`shaping_suite_scenarios_carry_a_retention_oracle`, so the suite cannot silently
stop measuring the thing it exists for.

## The command

```
./amber-bench run --live --suite shaping --repeat 5 --format text --out shaping-live.json
```

`--repeat 5` is the point: one run cannot tell a distribution from a typo. The
text report prints the score as `median (σ)` and the tokens as
`prompt median (σ, lo..hi)`; the JSON report carries `repeat_scores`,
`repeat_prompt_tokens` and `repeat_completion_tokens` per scenario.

## Prefill accounting

The suite counts what the client is told: `prompt_tokens` and
`completion_tokens` from the endpoint's usage block. An OpenAI-compatible
endpoint does not expose *prefill* separately from prompt tokens, and this one
reports no cache accounting, so the saving from a compaction or a trim is
**structurally verified and not measured**: the harness can prove the request it
sends is smaller, not that the server charged less for it. A provider that
exposes cached tokens can be added here without changing the scenarios.

## Baseline

Recorded 2026-10-28 against the endpoint amber is configured against — the
`kilocode` provider, model `kilo-auto/free` (the local `llama-turboq` service was
inactive; nothing here touched or reconfigured it). The benchmark uses the same
selection a host does (`agent::apply_global_selection`), so this is the
configuration a user runs, not a repository-local fallback.

```
./amber-bench run --live --suite shaping --repeat 5 --repeat-gap-ms 20000 \
  --format text --out shaping-live.json
```

| Scenario | score median (σ) | prompt tokens median (σ, range) | completion median (σ, range) | steps | wall |
|---|---|---|---|---|---|
| s-01-stale-early-evidence | 69.5 (0.0) | 72,873 (0.0, 72,873..72,873) | 569 (273.1, 486..1141) | 6 | 10.3 s |
| s-02-superseded-read | 81.5 (0.0) | 60,067 (4.1, 60,061..60,071) | 294 (7.6, 279..299) | 5 | 9.0 s |

Model score 755/1000, 5/5 measured runs per scenario, 0 unmeasured.

What the run shows:

- **Retention holds**: in `s-01` the deployment token read once at the start came
  back after five more reads, in every run.
- **The trim is reachable and safe**: in `s-02` the model called
  `context {"op":"superseded"}` on its own after the re-read, the call was
  allowed, and the answer still named the current version (`v2`).
- **The saving is not visible here**: the trim lands at the *next* request
  boundary, and this scenario ends its turn right after asking. The score and
  the answer are the evidence; the token delta needs a turn long enough to make
  another request, which `s-01` is (it does not trim) — the two scenarios
  bracket the behaviour rather than proving a saving.

### Findings from recording it

Two harness bugs produced the first, wrong baseline, and both are fixed:

1. **The benchmark did not use the configured provider.** Live mode loaded
   `amber.conf` directly, and this repository's file pins
   `api_base=http://127.0.0.1:8081/v1`, so a "live" run dialled the inactive
   local service while every host was talking to the configured provider. Live
   mode now applies the global provider selection first and treats the project
   file as a base layer that cannot redirect provider traffic (the same rule
   `Config::load_workspace_defaults` enforces), shared with the CLI and TUI
   through `agent::apply_global_selection`.
2. **The benchmark had no shaping queue.** It never attached `HostServices` to
   its plugin runtime, so `tool_context` correctly declined to install and a
   model that asked for `superseded` got `unknown tool: context`. The queue is
   wired now, and the agent drains it.

The shared provider-resolution module reads the user's global config, which is a
path derived from `XDG_CONFIG_HOME`; CodeQL flags that as `cpp/path-injection`,
exactly as it already flags the identical reads in `lib/config.cpp`,
`tui/window_manager.cpp` and `lib/skill_ledger.cpp`. The path is resolved by
`config_paths()`, which accepts an `XDG_CONFIG_HOME` only when it is absolute and
free of `..`; the two alerts were dismissed as that same sibling, not because a
new sink is safe to ignore.

One endpoint fact belongs here because it changes how the suite is run: the free
tier throttles a burst. A 5x repeat with no pacing produced 7 unmeasured runs
out of 10 (`HTTP 429`), and those runs are reported by count rather than folded
into the token spread as zeros — `--repeat-gap-ms 20000` measured 10/10.

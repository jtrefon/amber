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

**Not recorded: the endpoint was unavailable.** At the time of writing
`llama-turboq` was `inactive (dead)` (`systemctl status llama-turboq`), and the
inference service is not something this project starts, stops or reconfigures.
That is a blocked measurement, not a result — there is no number here to
misread as a finding.

To record the baseline, with the service up as configured:

1. `./amber-bench run --live --suite shaping --repeat 5 --format text --out shaping-live.json`
2. Append the report's per-scenario lines (score median/σ, prompt tokens
   median/σ/range, steps, wall) below, with the model name and the date.
3. If the model never calls `context`, say so: the trigger is the model's, and
   a model that ignores it measures retention without shaping. That is a finding
   about the model, not about the harness, and it belongs here rather than being
   silently averaged away.

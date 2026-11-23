# 0.9.0 live baseline - NOT MEASURED

The 0.9.0 live release artifact has **not** been produced. The release gate is
expected to print `NOT MEASURED` until a valid artifact is committed here.

## Run 1 (2026-11-23): full corpus, journey suites included

- Command: `amber-bench run --live --profile deepseek-chat --cat model --repeat 3`
- Corpus: 79 scenarios (the 56 model scenarios plus 18 `e2e` and 5 `repo-long`
  journey suites), cloud `deepseek-chat`.
- Result: 43/79 passed, model score **687.8/1000** (95% CI ±12.4).
- The six e2e workspace/empty-reason failures showed that the `e2e` and
  `repo-long` journeys are not part of the release model corpus; they have
  their own `agent-e2e` and `long-horizon` gates. The artifact was not
  committed.

## Run 2 (2026-11-23): 56-scenario model corpus

- Command: the same, after excluding `e2e` and `repo-long` from the model axis.
- Result: 23/56 passed, model score **648.4/1000** (95% CI ±15.9).
- The failures end with `HTTP 402 from LLM server: Insufficient ...`; the
  DeepSeek account balance was exhausted. This artifact is invalid as a release
  measurement and was not committed.

The fixed local `:8081` service is inactive and inviolate; no service change was
made. A valid 0.9.0 artifact will be produced after the DeepSeek account is
topped up (or the local service is available), then checked with:

```sh
make bench-release-check VERSION=0.9.0
```

Both runs are reported under the re-run policy: a repeat is justified by an
infrastructure/account failure, and the invalid run is not re-rolled silently.

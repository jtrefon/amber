# 0.8.8 live baseline

**Measured:** 2026-11-09, engine 0.8.8, repeat 3, cloud profile `deepseek-chat`
(`https://api.deepseek.com/v1`), 51 live scenarios, 37 passed, model score
**657.9/1000** (95% CI ±16.1). Artifact:
`cloud-deepseek-chat-r3.json`. `make bench-release-check VERSION=0.8.8` is
green.

The fixed local `llama-turboq` service was inactive for the whole session and is
inviolate: it is never started, stopped, reconfigured or replaced without
explicit human approval. This release therefore records one configured cloud
profile as the live minimum, as `docs/release-benchmark.md` allows when a key is
configured. The local fixed profile is still the full-population review target;
this artifact does not claim to be it.

The run was produced with an isolated XDG config selecting the DeepSeek provider
(`deepseek-chat`, thinking off, temperature 0.2), so no user config was edited:

```sh
XDG_CONFIG_HOME=<temp config with provider=deepseek, model=deepseek-chat> \
  ./amber-bench run --live --profile deepseek-chat --repeat 3 \
    --out bench/results/releases/0.8.8/cloud-deepseek-chat-r3.json
```

When the local service is confirmed up, the local run remains:

```sh
./amber-bench run --live --profile qwen38-27b-mtp --repeat 3 \
  --out bench/results/releases/0.8.8/local-qwen38-27b-mtp-r3.json
make bench-release-check VERSION=0.8.8
```

A model losing scenario points is the score (10 adherence and 5 oracle failures
here are model-quality rows). The gate only rejects infrastructure failures,
missing artifacts, wrong engine/model, a short corpus, or a score drop beyond
the band.

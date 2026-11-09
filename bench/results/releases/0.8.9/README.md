# 0.8.9 live baseline

**Measured:** 2026-11-09, engine 0.8.9, repeat 3, cloud profile `deepseek-chat`
(`https://api.deepseek.com/v1`), 51 live scenarios, 37 passed, model score
**675.9/1000** (95% CI ±11.3), **+18.0** against 0.8.8. Artifact:
`cloud-deepseek-chat-r3.json`. `make bench-release-check VERSION=0.8.9` is
green.

The fixed local `llama-turboq` service was inactive and is inviolate, so the
release records one configured cloud profile as its live minimum, as
`docs/release-benchmark.md` allows when a key is configured. The local fixed
profile remains the full-population review target; this artifact does not claim
to be it.

The run used an isolated XDG config selecting the DeepSeek provider
(`deepseek-chat`, thinking off, temperature 0.2), so no user config was edited:

```sh
XDG_CONFIG_HOME=<temp config with provider=deepseek, model=deepseek-chat> \
  ./amber-bench run --live --profile deepseek-chat --repeat 3 \
    --out bench/results/releases/0.8.9/cloud-deepseek-chat-r3.json
```

When the local service is confirmed up, the local run remains:

```sh
./amber-bench run --live --profile qwen38-27b-mtp --repeat 3 \
  --out bench/results/releases/0.8.9/local-qwen38-27b-mtp-r3.json
make bench-release-check VERSION=0.8.9
```

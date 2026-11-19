# Spec: Trace replay and capture

Status: implemented (#637, #638; gated by `make trace-check`).

A trace is a checked-in provider exchange: the request shape that must arrive
and the raw response bytes that go back. Replaying one exercises the real
`HttpLLMClient` and a real `Dialect` (OpenAI, Anthropic or Gemini) with no
network and no live model. The recorder is the only live path, and it is
manual.

## File schema

```json
{
  "schema": 1,
  "provider": "openai",
  "dialect": "openai",
  "description": "free text",
  "model": "optional, required for path-based Gemini fixtures",
  "source": "where the bytes came from (captured, authored from the wire reference, ...)",
  "redaction": "clean",
  "exchanges": [
    {
      "id": "tool-call",
      "match": {
        "path": "/v1/chat/completions",
        "stream": false,
        "roles": ["system", "user"],
        "tools": ["read"],
        "system": "Your name is **Amber**"
      },
      "request": { "model": "fake", "messages": [] },
      "response": {
        "status": 200,
        "content_type": "application/json",
        "body": "{\"choices\":[]}"
      }
    }
  ]
}
```

The full field contract, including the `source`/`redaction` requirement and the
SSE rules, is in `bench/traces/README.md`. A fixture is untrusted input to the
validator; `fuzz/trace_load.cpp` feeds arbitrary JSON to `parse_trace` under
SANITIZERS, and the target fails closed when the compiler has no libFuzzer.

- `dialect` selects the client; `provider` is a display name.
- `source` names the origin; it is required so a trimmed example cannot read as
  a capture. `redaction` is `clean` or `redacted`; any other value is rejected.
- `match.path`/`stream` are the wire shape. `roles` is the full message-role
  sequence (a top-level Anthropic `system`/Gemini `systemInstruction` counts as
  the leading `system` role). `tools` names must be offered. `system` is exact
  in strict mode and a required substring in shape mode.
- `request` is optional. Strict compares the whole body; shape ignores it. A
  captured trace omits `request` when redaction changed it, because the agent
  would not send the redacted value.
- `response.body` is replayed verbatim. JSON bodies must parse; SSE bodies must
  be blank-line-terminated frames whose `data:` payloads are JSON or `[DONE]`.
  A stream that ends without `[DONE]` is valid; a frame left open is not.

## Replay

`TraceResponder` serves the exchanges in order. It fails closed: a missing
exchange, an extra request, a drifted path, stream flag, role sequence, tool or
system text, an unused exchange, or (strict) a body diff all fail the run with
the file name and the exchange index. The endpoint answers `500` for a drifted
request and the first error is the one reported.

`amber-bench run` wires the responder to a loopback endpoint and the real
client:

```sh
amber-bench run --scenario trace-openai-read \
  --trace bench/traces/openai-read.json --trace-mode shape
amber-bench run --driver binary --scenario trace-openai-read \
  --trace bench/traces/openai-read.json --trace-mode shape
```

For a recorded trace use `shape`: the full system prompt carries the volatile
environment card, so the recorder writes only its stable opening line as the
`match.system` marker. Strict replay of a recording is possible only in an
identical workspace.

## Capture

```sh
amber-bench record --live --scenario <name> --profile <profile> \
  --trace-out bench/traces/<name>.json \
  --trace-source "captured from <provider> <model> on <date>; prompt trimmed to one turn"
```

The recorder starts a loopback proxy, points the scenario at it, forwards each
chat request to the live provider and keeps the request JSON plus the raw
response bytes. `--trace-out` requires `--live`, one `--scenario` and the
in-process driver; a hermetic run is refused.

- Request headers and query keys are never written.
- Credential-shaped values (OpenAI `sk-`, AWS `AKIA`, Google `AIza`, GitHub,
  Slack, private-key headers, bearer tokens) are replaced with `[redacted]` and
  the file records `redaction: "redacted"`.
- The serialized document is scanned again before an atomic rename. A
  credential that survives (for example in the author-supplied source note)
  makes the recorder refuse and write nothing.
- The model is read from the request body or a Gemini path and stored at the
  root.

CI never runs `record` and never calls a provider. The committed corpus is the
only input.

## Gate

```sh
make trace-check
```

runs `RUN_TESTS_ONLY=trace ./bench_test` — which loads every committed fixture,
synthesizes a request per dialect from its match spec and replays it through
`TraceResponder` with exact status/content-type/bytes — then one scenario
through the real client. The corpus is in `bench/traces/`: OpenAI (tool calls,
streams, 429, malformed JSON, truncated SSE, empty choices, unknown/duplicate
calls), Anthropic (content blocks, cache counters, tool use, overloaded,
malformed event, empty content) and Gemini (function call, usage, empty
candidate, 429, malformed JSON, stream).

## Adding a trace

1. Record it with `amber-bench record` (or author it from the provider's wire
   format and say so in `source`). Keep one exchange per chat request, in order.
2. Check `redaction`, `source`, `dialect` and (for Gemini) `model`.
3. Add a replay test if the fixture introduces a new wire shape; the directory
   scan already covers load/replay. `make trace-check` must pass.
4. Run `make fuzz-run` (or at least `make fuzz`) when the parser/validator
   changed; the trace target is part of the five-target fuzz job.
5. Never commit a fixture that the scan flags. If the recorder refuses, fix the
   source or redact it; do not weaken the rule.

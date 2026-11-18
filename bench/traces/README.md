# Provider trace replay

A trace is a checked-in recording of a provider exchange: the request
fingerprint that must arrive, and the response bytes that go back. Replaying one
exercises the real `HttpLLMClient` and a real `Dialect` (OpenAI, Anthropic or
Gemini) with no network and no live model.

```sh
amber-bench run --scenario trace-openai-read \
  --trace bench/traces/openai-read.json --trace-mode shape
amber-bench run --driver binary --scenario trace-openai-read \
  --trace bench/traces/openai-read.json --trace-mode shape
```

`make trace-check` loads and replays every fixture in this directory through
`TraceResponder`, then runs one scenario through the real client. CI never
records and never calls an endpoint; the corpus is the only input.

## Recording

`amber-bench record` is a manual, live action. It starts a loopback proxy, points
the scenario at it, forwards each chat request to the real provider, and writes
the request bytes plus the raw response bytes:

```sh
amber-bench record --live --scenario <name> --profile <profile> \
  --trace-out bench/traces/<name>.json \
  --trace-source "captured from <provider> <model> on <date>; prompt trimmed to one turn"
```

- `--trace-out` requires `--live`, one `--scenario`, and the in-process driver.
  A hermetic run is refused: there is no provider to record.
- `--trace-provider` and `--trace-dialect` override the labels derived from the
  live config (`openai`, `anthropic`, `gemini`).
- Request headers are never written. Authorization, `x-api-key`, cookies and
  `key=` query parameters are forwarded upstream and dropped at the boundary.
- A response or request value matching a known credential shape (OpenAI `sk-`,
  AWS `AKIA`, Google `AIza`, GitHub, Slack, a private-key header, a bearer token)
  is replaced with `[redacted]` and the file records `"redaction": "redacted"`.
- Before writing, the whole serialized trace is scanned again. If a
  credential-like value survives (for example in the author-supplied source
  note), the recorder refuses and writes nothing. **A trace that cannot pass the
  scan is not committed.**
- The model is read from the request body, or from a Gemini path, and stored at
  the root so path-based dialects replay against the right URL.

## Schema

```json
{
  "schema": 1,
  "provider": "openai",
  "dialect": "openai",
  "description": "free text",
  "model": "gemini-test",
  "source": "captured from ... on 2026-11-18",
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

- `schema` is the integer `1`.
- `dialect` is `openai`, `anthropic` or `gemini`; it selects the client
  implementation. `provider` is a display name and defaults to `dialect`.
- `source` says where the bytes came from (captured, authored from the wire
  reference, ...). It is required: a trimmed example must not read as a capture.
- `redaction` is `clean` or `redacted`; any other value is rejected. Both say
  the scan ran, not that a secret was found.
- `model` is optional. Gemini puts the model in the path, so its fixtures set
  it; OpenAI and Anthropic take it from the request body.
- `exchanges` is a non-empty array, served in order, one per chat request.
- `match.path` is the request path; `match.stream` is the request's stream mode.
- `match.roles` is the full message-role sequence. Anthropic's top-level
  `system` and Gemini's `systemInstruction` count as the leading `system` role.
- `match.tools` lists tool names that must be offered. An OpenAI
  `function.name`, an Anthropic `name` and a Gemini `functionDeclarations` entry
  all count.
- `match.system`: in `strict` mode the system text must match exactly; in
  `shape` mode it must contain the string. Shape also ignores the prompt text,
  so a rewritten user turn still replays as long as the wire shape holds.
- `request` is optional. In `strict` mode it is compared with the whole request
  body and the first difference is reported by JSON path. It is omitted when
  redaction changed it, because the agent would not send the redacted value.
- `response.body` is replayed verbatim. `application/json` bodies must parse;
  `text/event-stream` bodies must be blank-line-terminated frames whose `data:`
  payloads are JSON (or `[DONE]`). A stream that ends without `[DONE]` is valid
  and is how a truncated stream is recorded; a frame left open is not, and the
  recorder refuses to write one it cannot replay.

## Failure behaviour

A trace fails closed. A missing exchange, an extra request, an unmatched path,
role, tool, system text or request body, and an unused exchange all fail the
scenario with the file name and the exchange index. The endpoint answers `500`
for a drifted request and the first error is the one reported.

`cache_breakpoints` is a scenario-level `request_contract` field. A trace
replays the wire bytes a provider sent; it does not invent them.

## Corpus coverage

The committed set covers the shapes a client must survive:

- OpenAI: non-stream and stream tool calls, 429, malformed JSON, a stream that
  ends without `[DONE]`, an empty choice list, an unknown tool, a duplicate
  tool call, and a plain answer with cache-free usage.
- Anthropic: JSON and streamed content blocks, cache read/write counters, tool
  use, 429/overloaded, a malformed event, and empty content.
- Gemini: function call, usage metadata, an empty candidate list, 429, malformed
  JSON, and a streamed text chunk.

Every fixture is replayed by `trace_corpus_replays_every_fixture`; the real
client is exercised against OpenAI, Anthropic and Gemini fixtures by separate
tests.

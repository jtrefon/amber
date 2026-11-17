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

## Schema

```json
{
  "schema": 1,
  "provider": "openai",
  "dialect": "openai",
  "description": "free text",
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
  body and the first difference is reported by JSON path.
- `response.body` is replayed verbatim. `application/json` bodies must parse;
  `text/event-stream` bodies must be blank-line-terminated frames whose `data:`
  payloads are JSON (or `[DONE]`).

## Failure behaviour

A trace fails closed. A missing exchange, an extra request, an unmatched path,
role, tool, system text or request body, and an unused exchange all fail the
scenario with the file name and the exchange index. The endpoint answers `500`
for a drifted request and the first error is the one reported.

`cache_breakpoints` is a scenario-level `request_contract` field. A trace
replays the wire bytes a provider sent; it does not invent them.

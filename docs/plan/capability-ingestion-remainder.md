# Plan: the remainder of #617 (capability ingestion)

- **Status:** proposal. No code yet. The reasoning half landed in #623
  (`b3e84792`); this plan covers what that PR explicitly left open.
- **Issue:** #617 (P2, umbrella). Each slice below should be its own PR, with a
  red test committed before production code.

## What is already true (verified against current `main`)

- `ReasoningContract` travels through `ModelInfo`/`ServerInfo`/`Config`; both
  dialects consume it, the provider-switch reset clears it, and the TUI
  validates the listing-owned effort levels (`include/agent/config.h:58`,
  `lib/dialect_openai.cpp:952`, `lib/dialect_anthropic.cpp:82`,
  `lib/providers_service.cpp:220`).
- Recorded DeepSeek bodies parse `context_window`, `max_output_tokens` and
  top-level modalities (`tests/reasoning_contract_test.cpp:60-99,214-217`).
  OpenRouter `supported_parameters` and top-level `reasoning`, OpenCode
  `capabilities.reasoning`, and llama.cpp `meta.n_ctx` are also read.
- The four defects listed in #617's cross-provider audit — `ServerInfo` dropping
  the contract, the request builder ignoring it, no reasoning replay, and the
  fixed UI vocabulary — were the work of #623 and are closed.

So the alias-table slice is now **structural, not behavioural**: the three
catalogues named as acceptance cases already resolve. Its value is making the
next provider a table entry, not a parser edit.

## Slice 1 — one alias table per capability concept

Replace the scattered `read_int`/`append_unique_strings` calls in
`lib/dialect_openai.cpp:452-535` with a documented alias list per concept:

| concept | aliases (in priority order) |
|---|---|
| context window | `meta.n_ctx`, `n_ctx`, `context_window`, `context_length`, `top_provider.context_length`, `limit.context` |
| training window | `meta.n_ctx_train`, `n_ctx_train`, `context_length` |
| output limit | `max_output_tokens`, `max_completion_tokens`, `top_provider.max_completion_tokens`, `limit.output` |
| input modalities | `input_modalities`, `architecture.input_modalities`, `modalities.input` |
| output modalities | `output_modalities`, `architecture.output_modalities`, `modalities.output` |

One dotted-path reader covers all of them; `parse_limits`, `parse_top_provider`,
`parse_modalities` and the duplicated reads in `parse_entry` collapse into it.
Existing recorded-body tests are the regression net. Red test: a body using a
spelling from a *different* provider for the same concept resolves identically
to the body that currently uses the native spelling.

## Slice 2 — `verbosity` as a listed capability

There is currently no `verbosity` identifier anywhere in `include/`, `lib/`,
`tui/` or `completions.json`. The work: record the wire shape (OpenRouter
advertises `verbosity` in `supported_parameters` on 153 entries; the concrete
request field and accepted levels still need a captured body), add it to
`ModelCapabilities`/`Config`, ingest it in the alias table, validate it in
`/set` the same way reasoning levels are validated, and emit it only for models
that advertise it. Acceptance: a listed model accepts exactly its advertised
levels; an unlisted model sends nothing.

## Slice 3 — Gemini `thinkingConfig`

`lib/dialect_gemini.cpp` has zero thinking/reasoning code. Google's wire shape is
`generationConfig.thinkingConfig.thinkingBudget`; the slice needs request
construction, response-part parsing, echo behaviour on tool turns and a
`ModelCapabilities` contract derived from Google's listing. Acceptance: a
configured thinking budget reaches the wire, thought parts are not sent back as
plain text, and compression preserves them when the contract requires echo.

## Recorded bodies needed before slices 2 and 3

The four catalogue bodies from the #617 audit and the DeepSeek one are partly
cached locally, but no verbosity-bearing body and no native Gemini listing is in
the tree. Add them to `tests/` as conformance fixtures first; a test against an
invented shape would pin a guess rather than the provider's contract.

## Verification for every slice

`./run_tests` (full suite), the PTY suite, `make tui-agent-e2e`,
`make verify-quick`, `make lint-changed BASE=origin/main`, and a recorded-body
conformance test that fails for a new provider instead of failing silently in
production.

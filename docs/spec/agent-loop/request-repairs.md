## Spec: Request Repairs

### Purpose

A model request can fail for a reason that is **our** fault rather than the
server's: the loaded template cannot parse the tool grammar, the configured
model does not exist on this server, or the key was rejected. Each is
non-retryable as sent and repairable by changing the request once. This spec
defines the ladder that decides which repair applies, so adding one is a unit
and a registration line, not an edit to the loop.

### Ownership

- **Source files**: `include/agent/repair.h` (`RepairHost`, `RepairStrategy`,
  `RepairLadder`, the factories), `lib/repair.cpp` (the three strategies and the
  default ladder), `lib/agent.cpp` (`Agent::RepairBridge`, the adapter that gives
  a strategy the agent's config/hooks/log, and `build_chat_adapter`).
- **Test files**: `tests/run_tests.cpp` (`repair_*`, against a fake host);
  `tests/agent_loop_test.cpp` (`agent_template_parser_400_recovers_without_tools`,
  `agent_loop_auth_401_recovers_with_new_key`,
  `agent_loop_auth_401_declined_key_degrades`).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | The failure text of a request that failed as sent, the stage it happened in, and whether the request carried tools |
| **Output** | A `RetryRequest`: no repair, retry without tools, or retry with the same tools |
| **Error states** | A strategy that cannot act returns `None`; the ladder then offers the failure to the next strategy of the same kind. Nothing here throws. |
| **Invariants** | See below. |
| **Thread safety** | Runs on the agent thread, inside the request's retry loop. |

### Invariants

1. **One repair per request.** The adapter hands the retry loop at most one
   alternative request; the repaired attempt runs with full retry semantics but
   is not itself repaired.
2. **The failure is recorded even when no repair applies.** "The server rejected
   the model" belongs in the error journal whether or not a replacement exists.
3. **A repair announces itself.** The model is told (`[harness]` note) that the
   request it asked for is changing shape, and why. Without that, a silent
   change of model reads as the model's own confusion.
4. **Strategies see only `RepairHost`.** No strategy holds an `Agent`, a
   `Context`, or the hooks, which is what makes each one testable against a
   fake.
5. **Order is registration order.** The ladder walks the strategies that handle
   the classified failure in the order they were added.
6. **A new repair is a new strategy plus one line** in
   `make_default_repair_ladder()`.
7. **A repair cannot widen what the model may do.** Dropping tools narrows the
   request; swapping the model or the key keeps the same tool set, the same
   policy, and the same workspace.

---

### Scenarios

#### [RP-01] The template cannot parse the tool grammar

- **Given**: a server that answers `400 Unable to generate parser for this template`
- **Input**: a request that carried tools
- **Expected**: the failure is recorded as `template_parser`; the model is told
  the request is retried without tools; the retry runs with an empty tool list
  and the turn continues
- **Regression guard**: `repair_template_parser_drops_tools_and_says_so`,
  `agent_template_parser_400_recovers_without_tools`

#### [RP-02] The configured model does not exist

- **Given**: a server that does not serve the configured model
- **Input**: a request, with or without tools
- **Expected**: the failure is recorded as `model_name`; the first model the
  server advertises is adopted (with its capabilities), the host is told, and
  the retry keeps the tools. If the server advertises nothing, or advertises the
  model already configured, no repair applies
- **Regression guard**:
  `repair_server_model_switches_to_the_first_advertised_model`,
  `repair_server_model_declines_when_it_already_uses_that_model`,
  `repair_server_model_declines_with_no_model_list`

#### [RP-03] The key was rejected

- **Given**: an HTTP 401/403 from the provider
- **Input**: a request
- **Expected**: the failure is recorded as `auth`; the host is asked for a
  replacement key for this stage. With a key, it is adopted and the retry keeps
  the tools; without one, no repair applies and the turn degrades as before
- **Regression guard**: `repair_api_key_swaps_the_key`,
  `repair_api_key_declines_when_the_host_has_none`,
  `agent_loop_auth_401_recovers_with_new_key`,
  `agent_loop_auth_401_declined_key_degrades`

#### [RP-04] A fourth repair joins without touching the loop

- **Given**: a new failure kind and a new strategy
- **Input**: the strategy is added to `make_default_repair_ladder()`
- **Expected**: the ladder picks it up; no change to `build_chat_adapter`, the
  turn, or the tool policy
- **Regression guard**: `repair_ladder_walks_past_a_strategy_that_cannot_act` (a
  strategy that returns `None` does not block a later one of the same kind)

# Spec: Agent simulation (policies, the state machine and the zoo)

Status: implemented (#631, #632; gated by `make bench-check`,
`make policy-matrix-check`).

A policy is a deterministic, closed-loop model of the provider. It sees the
conversation and the last tool result before it answers, so a penetration test
can assert that the harness reacts to what the tools did instead of replaying a
fixed transcript.

## Contract

`bench::Policy` is one method:

```cpp
struct PolicyContext {
    int step;                                    // model request index
    const std::vector<agent::Message>* messages;
    const std::vector<std::shared_ptr<agent::Tool>>* tools;
    const Scenario* scenario;
    std::string last_tool_name;
    std::string last_tool_status;                // ok | error | denied | timeout | empty
    std::string last_tool_result;                // the tool message content
};
class Policy {
    virtual PolicyReply next(const PolicyContext&) = 0;
};
```

`PolicyReply` carries `content`, `tool_calls`, `reasoning`, `error`,
`retryable`, `latency_ms`, `drop_after_chunks` and the usage counters.
`bench::PolicyClient` adapts it to `agent::LLMClient`; it fills `Stats` from the
reply and exposes `chat_calls`/`tool_counts` for a scenario to assert request
counts.

## The state machine

`make_state_machine_policy(json, err)` compiles the JSON used by a scenario's
`policy` field. The initial state and every transition are validated before a
run: an unknown state, a missing terminal, an unbounded cycle, an unknown
condition or a malformed action fails loading, not the run.

```json
{
  "initial": "read",
  "max_transitions": 8,
  "states": {
    "read":  {"call": {"tool": "read", "args": {"path": "a.txt"}}, "next": "done"},
    "done":  {"final": "done"}
  }
}
```

| state | fields |
|---|---|
| `call` | `call.tool`, optional `call.args` (object or string), `next` |
| `final` | `final` (assistant text) |
| `fail` | `fail` (error text; the reply is non-retryable) |
| any | `when` guard; `on_error` at the policy root with `retry`/`next` |

Conditions are `last_tool_status`, `last_tool_name`, `last_result_contains`,
`step` and `always`. A rejected guard leaves the state through
`on_error.next` while the retry budget lasts, then returns a policy failure.

```json
{
  "initial": "test",
  "max_transitions": 10,
  "on_error": {"retry": 1, "next": "fix"},
  "states": {
    "test":   {"call": {"tool": "bash", "args": {"command": "make test"}},
               "next": "check"},
    "check":  {"when": {"last_result_contains": "passed"}, "final": "done"},
    "fix":    {"call": {"tool": "write",
                        "args": {"path": "a.cpp",
                                 "edits": [{"old": "old line", "new": "fixed line"}]}},
               "next": "retest"},
    "retest": {"call": {"tool": "bash", "args": {"command": "make test"}},
               "next": "done"},
    "done":   {"final": "done"}
  }
}
```

The `check` final is guarded: after the first `make test`, a failure routes
through `on_error.next` to `fix`, while a pass ends the run. `on_error.retry`
bounds the repair attempts; without it a rejected guard is a policy failure.

`PolicyReply.tool_calls` are built with a deterministic id derived from the
step; a state machine never invents an id from randomness.

## The zoo

The fixed policy zoo is `bench/policy_zoo.cpp`: `planner`, `lazy`, `wrong_tool`,
`hallucinator`, `quitter`, `over_eager` and `retrier`. `bench/policies/expectations.json`
records the point expectation for each on the `zoo` scenario (success, score
bounds, the failure text it must contain). `check_policy_expectations` runs as
part of `amber-bench run --policy-matrix`.

Point expectations cannot see a matrix that stopped discriminating: every
policy can land on the right side of its own threshold while the scores
converge. `bench/results/policy-matrix-baseline.json` records the best-vs-worst
score gap per scenario; `tools/policy_matrix_check.py` fails below 90% of the
recorded gap (0.5 absolute slack).

```sh
amber-bench run --policy-matrix --scenario zoo --out artifacts/policy-matrix.json
python3 tools/policy_matrix_check.py --report artifacts/policy-matrix.json \
  --baseline bench/results/policy-matrix-baseline.json
# or, with the report and summary paths the gate uses:
make policy-matrix-check
```

## Using a policy

A scenario opts in with a `policy` object; it needs no `fake_replies`. The
in-process runner constructs the `PolicyClient` when the scenario has a policy
(or the run names a zoo policy with `--policy`) and no trace override. The
binary/CLI tier cannot run a C++
policy: it uses the scripted endpoint or the trace responder, so a journey that
must run through `amber-cli` uses `fake_replies`/`endpoint_replies`.

## Adding a policy

1. Write the state machine in the scenario JSON (or extend `policy_zoo.cpp` for
   a reusable member). Keep `max_transitions` bounded; the validator rejects an
   unbounded cycle.
2. Add a scenario under `bench/scenarios/policy/` or the suite that owns the
   behaviour. Declare `oracle`, `checks`, `optimal_plan` and a generous
   `budget`; a policy that loops must fail by the transition/step cap.
3. Run it red first: `amber-bench run --scenario <name>` must show the missing
   behaviour. Then make the policy or harness change and run it green.
4. If the policy joins the zoo, update `bench/policies/expectations.json` and
   regenerate the baseline only after review:
   `make policy-matrix-baseline`.
5. Run `make policy-matrix-check` and `make bench-check` before the PR.

A policy is a test double, not a model. It does not prove what a live model
would do; it proves that the harness, the tools and the context react correctly
to a deterministic sequence. Live prompt evaluation stays manual
(`BENCHMARK.md`).

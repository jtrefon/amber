# Long-horizon repository journeys

These scenarios exercise the harness on a multi-module change with a real build
and test loop, not a single-file edit.

- Each template (`math/`, `text/`, `inventory/`, `report/`) has a `skeleton`
  copied into the workspace, a `reference` used by `validate-template`, and
  `hidden_tests/` compiled against the workspace after the run. The skeleton's
  root `.cpp` files are the contract the hidden harness compiles; `tests/` is
  the visible suite the agent runs with `make test`.
- `l-01-math`, `l-02-text`, `l-03-inventory` and `l-04-report` are closed-loop
  state-machine policies: read the task and the source, write the fix, run
  `make test`, answer. `l-05-text-followup` models a first attempt that fails
  the visible test and a follow-up that fixes it; its scripted replies are the
  deterministic proof of that failure/repair sequence.
- `expected_workspace.files.contains` asserts the edited source. A wrong edit
  fails with a message naming the file and the missing text; the inverse case
  (the diff assertion) is covered by
  `long_horizon_wrong_edit_is_named_in_the_workspace_diff` in `bench_test`.

`make long-horizon-check` runs the suite and compares per-journey tokens, tool
calls and wall time with `bench/results/long-horizon-baseline.json`; the
baseline is regenerated with `make long-horizon-baseline` after a reviewed
change. The policy client reports no provider usage, so its four scenarios
record 0 tokens; the tool-call and wall axes are the recorded regression
signals for those. The template compiles are hermetic and run on Linux and
macOS.

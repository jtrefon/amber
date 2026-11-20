# Plan: Shift+Tab mode cycle and the tab-switch reproduction gap

- **Status:** #612 implemented; #621 remains a reproduction gap. The tab-close
  work from the original plan landed separately in #675 (`e8a320a6`), so this
  branch no longer carries it.
- **Issue:** #612 (Shift+Tab cycles the agent mode).
- **Regression guard:** the agent journey for #621.

## #612 — Shift+Tab is a bound action

`keybindings.json` gains a global `shift+tab -> ui.mode.cycle` row, and
`docs/spec/tui/keybindings.md` extends its key vocabulary with that spelling.
`KeyBinder::dispatch()` maps `tui::keys::kBtab` (already in the L1 vocabulary
and verified against `KEY_BTAB`) to `shift+tab`, so the one binder/table/action
path owns the shortcut rather than a new branch in the input loop.

A pure `agent::next_mode()` helper expresses `read -> write -> yolo -> read` in
one place. `SlashDispatcher` registers `ui.mode.cycle` as a closure that reads
the active agent's published config snapshot (falling back to `Tui::cfg_`),
computes the next mode through that helper, and calls the existing
`apply_policy_mode()`. The host template, the active agent's approval gate and
the status-bar snapshot therefore change together; `handle_key_binding()`
already repaints on a handled action, so the indicator updates immediately.
The slash commands in `completions.json` remain the direct selector, and the
menu/keybinding conformance test's known-`ui.*` list names the new action.

## #621 — not reproducible here

The agent journey drives the reported path on Linux with `TERM=xterm-256color`:
a slow in-flight model call, a real SGR mouse click on the other tab, then a
keystroke with no mouse input, followed by the result landing in its own tab.
It passes on current `main`; the code audit found no deterministic broken
invariant. Writing a fix without a reproducing test would be a guess, so the
journey stays as a regression guard and the issue remains open. A macOS
recording (terminal app, tmux/screen, streaming vs reasoning, and the frame at
the freeze) is what would move it forward.

## Verification

- Unit suite: `./run_tests` — 2098 passed, 0 failed.
- PTY suite: `./tui_pty_test ./amber` — green, including
  `shift_tab_cycles_the_agent_mode`; `--repeat 2` green on both passes.
- Agent journeys: `make tui-agent-e2e REPEAT=2` — green twice.
- Static gates: `make verify-quick` clean; `make lint-changed BASE=origin/main`
  0 findings; pinned-container cppcheck clean.
- Hands-on E2E on a pty: the status segment moved
  `write -> yolo -> read -> write -> yolo` on four Shift+Tab presses and
  `/get policy mode` agreed with the indicator.

Full-tree `make lint` did not finish inside 30 minutes on this host; the
incremental gate and the pinned-container analysis are the local evidence, and
CI's `static-gates` job is the full-tree check.

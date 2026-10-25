# Headless one-shot TUI (Track B)

**Status: deferred, not implemented.** Recorded 2026-10-25 so the design does not
have to be re-derived. It is not a prerequisite for the coding-library work: the
live eval runs through `amber-cli`, which is a full core host (plugins, prompt
registry, skills, memory, tools, JSONL transcript).

## The problem

Two needs share one shape:

1. **Full-host end-to-end tests.** The TUI host adds what `amber-cli` does not:
   the window manager and its per-window agent, prompt-registry attachment,
   session save/restore, slash commands, panels, the status bar, worktrees. A
   library fix can be verified through the CLI, but a TUI regression needs the
   TUI.
2. **One-off fully configured amber.** A user with a complete TUI configuration
   may want one prompt run and a clean exit — from cron, an editor, or CI —
   without driving ncurses by hand.

The existing PTY harness (`tests/tui_pty_test.cpp`) already spawns the real
binary, sends keys, and restores the terminal. It is the right tool for key
routing, but screen parsing is brittle for answer-level assertions.

## Design

```
amber --prompt "<text>" --exit-after-turn [--ephemeral]
```

- **Same construction, same loop, same teardown.** Inject the prompt into the
  active chat window and set a quit condition when that window's turn ends. No
  parallel runner: a one-shot path that diverges from the interactive path stops
  being an end-to-end test of the real thing.
- **`--ephemeral`** skips session persistence (or writes under a throwaway state
  root), so CI does not pollute `~/.local/state`. The PTY fixture already
  isolates `XDG_CONFIG_HOME`/`XDG_STATE_HOME`/`XDG_CACHE_HOME`
  (`tests/tui_pty_test.cpp:395-401`); reuse that pattern.
- **PTY in CI.** ncurses still runs; the harness provides the terminal. The
  flag replaces keystroke scripting, not the terminal.
- **Normal mode is the product feature.** Without `--ephemeral`, the run leaves
  the same session and transcript a hand-driven turn would. That is the "one-off
  fully configured amber" use case.

## Deliberately not in v1

- **A scripting language inside the TUI.** `--script` with `/set`, prompts and
  timing is a second command surface with approval and security implications,
  and a harness that is not the user path. If command sequences are needed
  later, the narrow extension is `--commands <file>` feeding the **existing
  slash dispatcher** — same parser, same approvals, same actions — not a new
  interpreter.
- **Config manipulation for tests.** Prefer ephemeral XDG roots: write the exact
  `settings`/`amber.conf`/plugin state the test needs, run, discard the tree.
  There is nothing to "disable after the prompt" because the world is thrown
  away. Debug/telemetry/reasoning are already config keys.

## Acceptance tests

- A PTY test runs `--prompt "reply with ok" --exit-after-turn --ephemeral`,
  asserts the answer and a clean exit, and asserts no session was written.
- The same without `--ephemeral` writes a session that a normal start restores.
- A turn with a tool call completes before exit (the quit waits for the turn,
  not the keystroke).
- The terminal is restored on every path, including a failed turn.

## Why deferred

The coding-library fix was verified with `amber-cli` plus a hermetic router eval;
building the TUI mode first would have delayed the fix for a host difference that
is not where the bug was. Revisit when a TUI-specific regression needs an
answer-level e2e assertion, or when the one-off product feature is wanted.

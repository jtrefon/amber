# Key-Binding Protocol Cleanup — target state for the parked TUI input-loop WIP

Status: **proposal, awaiting sign-off.** No production code in this change yet —
`AGENTS.md` requires Red → Proposal → Sign-off → Green → PR, and the RED commit is
step 1, not this document.

Source: `wip/stash0-tui-refactor` (your parked TUI refactor, `stash@{0}`).

## Problem

`Tui::run()` was a 443-line, 95-branch god method. You began splitting it. That work
is **largely finished** by other means, and the parked diff no longer describes the
right change — it mixes three things with three different verdicts, and one of them is
now actively wrong.

Rebasing 680 changed lines in `tui.cpp` to achieve a goal that has since been reached
is pure risk for no benefit. This proposal says: **discard the superseded part, keep
the ~20% that is still the right change, and re-derive it against today's code.**

### What the parked diff actually contains

Measured against `main` today, not against its own 101-commit-old base:

| # | Change in the WIP | Verdict on today's `main` | Evidence |
|---|---|---|---|
| 1 | Extract 7 helpers from `run()` (`handle_tick`, `handle_keybinder_key`, `handle_command_result`, `show_help_page`, `update_completions`, `handle_signal_shutdown`, `redraw`) | **Superseded — drop** | `Tui::run()` is **37 NLOC / 12 CCN** (`tui/tui.cpp:444`), inside the 40/14 limits. `handle_mouse_wheel` **already exists** (`tui/tui.cpp:764`). |
| 2 | Delete `KeyAction::{CloseWindow, Quit, Scroll, RouteToCommandLine}` | **Still valid — keep** | `KeyBinder` emits exactly 6 values (`CancelOrQuit`, `CloseDrawer`, `DeleteWord`, `NewWindow`, `SwitchWindow`, `ToggleScrollMode`). **None of the 4 are ever produced, and none are consumed anywhere.** |
| 3 | Delete `InputState::{scroll_mode, window_count, has_pending_prompt}` | **Now wrong — drop** | These have **8 / 2 / 1 live uses** on `main`. Deleting them breaks the input loop. |
| 4 | Add `OpenPanels` typed intent (Alt+0 / ESC+0) | **Still valid — keep** | Alt+0 is handled **inline** at `tui/tui.cpp:466` as a special case in `run()`, bypassing the typed-intent protocol. |

Also carried: `KeyAction::text` is unused (`key_binder.cpp` never writes it; the `.text`
hits elsewhere are `CommandLine::text()`), so dropping the field is free.

## Target state

**The key-binding protocol carries exactly the intents that exist, and nothing is
special-cased inside the input loop.**

```
KeyBinder::dispatch(key, state) -> KeyAction    // total: every enum value has a producer
        |
        v
Tui::apply_key_action(action)                   // exhaustive switch; no inline Alt+N handling
```

Three properties, each one checkable:

1. **No dead variants.** Every enumerator in `KeyAction::Type` is produced by
   `KeyBinder` and consumed by `apply_key_action`. This is the invariant the four dead
   values violated.
2. **No inline key special-cases in the loop.** Alt+0 reaches `open_panels` through a
   `KeyAction`, not through a branch inside `run()`.
3. **The enum stays an intent vocabulary, not an event log.** `arg` carries the window
   index; `text` goes, because nothing ever used it.

`Tui::run()` and `apply_key_action` are left structurally alone. They are within
limits, and the decomposition already landed.

## Why this shape, against our standards

- **Typed intents over hardcoded branches.** `AGENTS.md` already requires this for
  slash commands ("no exceptions"): behaviour lives in registered handlers, and paths
  are never special-cased in the dispatcher. Alt+0 currently violates that by being a
  raw branch in `run()`. This change removes the violation rather than inventing a
  parallel mechanism.
- **OCP.** Adding `OpenPanels` is a new enumerator plus a producer, with no edit to
  the dispatch algorithm — the enum is an open extension point.
- **Size limits.** No function grows. `run()` (37/12) and `apply_key_action` (35/12)
  stay under 40/14, and `KeyBinder::dispatch` shrinks as variants are removed.
- **Dead code is a gate concern, not a style preference.** `misc-unused-*` cannot see
  a never-constructed enum variant; only a human removing it can. That is why this is
  a proposal and not a lint fix.
- **Boy Scout.** The `InputState` fields stay because they are *used*. Removing them
  is not cleanup; the WIP's version of that was written against code that has since
  gained users.

## The invariant, made enforceable

The lesson from this whole audit is that a stated rule nobody checks is not a rule.
So step 1 (RED) is a test, not an edit:

**`TEST(key_action_has_no_unproduced_variants)`** — enumerate every value in
`KeyAction::Type` and assert each is produced by some `KeyBinder` binding or is on an
explicit, commented allowlist of reserved values.

Today it **fails**, listing the four dead variants. After the change it passes, and a
future variant added without a producer fails the suite the day it is written — rather
than sitting dead until someone notices.

This is the same shape as `build-hygiene` P9/P10 and `gate_needs_check --selftest`:
the rule lives next to the code, has cases, and is checked by `make check`.

## Plan

| Step | Commit | Content |
|---|---|---|
| 1. RED | `test: assert every KeyAction variant has a producer` | The invariant test. Fails, listing 4 variants. No production code. |
| 2. — | *this document* | Proposal + sign-off gate. |
| 3. GREEN | `refactor: drop dead KeyAction variants and route Alt+0 through the protocol` | Delete the 4 variants + `text`; add `OpenPanels`; move the `tui.cpp:466` branch into `apply_key_action`; add the `KeyBinder` binding for Alt+0 / ESC+0. |
| 4. REFACTOR | — | `redraw(cl)` if step 3 shows duplication worth collapsing. **Deferred** — not in scope, and `run()` does not need it. |

Expected diff: **~60 lines across 3 files**, versus 680 in `tui.cpp`.

### Explicitly out of scope

- Rebasing `wip/stash0-tui-refactor`. Its superseded 80% is discarded, not merged.
- `InputState` field removal — those fields are used.
- `tui_input.cpp` (2987 lines). It is over the audit table's 3000-line guidance and
  deserves its own proposal; bundling it here would repeat the mistake this document
  exists to prevent.
- The `stash@{1}` website/docs work (103 files, base 102 commits behind). Separate,
  and it needs the same discard/re-derive treatment.

## Risks

| Risk | Mitigation |
|---|---|
| A dead variant is actually reachable through a path I did not trace | The RED test enumerates producers from `KeyBinder`; if any variant is reachable elsewhere, the test is wrong and says so. Compile plus 899 tests plus the pty/e2e harness cover key paths. |
| Removing `text` breaks an aggregate initialiser | `KeyBinder` constructs `{type, arg, ""}` in 8 places; all are updated in the same commit. |
| Alt+0 behaviour changes subtly | It moves from a branch in `run()` to an intent; the observable behaviour must be identical. Add a TUI test asserting Alt+0 opens the panel view. |

## Sign-off needed

Two questions before step 1:

1. **Do you want `OpenPanels` at all?** Alt+0 already works via an inline branch, and
   `core.panel` is already a registered action (`tui/tui_input.cpp:1187`). Routing it
   through `KeyAction` is about consistency with the typed-intent rule, not about
   fixing a bug. If you would rather not add the variant, step 3 shrinks to
   *dead-code removal only* — smaller, and still worth having.
2. **Is discarding the input-loop extraction right?** My evidence is `run()` at 37
   NLOC / 12 CCN with `handle_mouse_wheel` already present. If you were also fixing
   something in that extraction I have not measured, say so and I will re-scope
   before writing a line of code.

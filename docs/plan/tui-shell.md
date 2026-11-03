# TUI shell — full ncurses interface

**Verified against:** amber 0.8.8 (`4b4274ab`), 2026-11-03.

- **Status:** Agreed 2026-11-02. Delivery plan for the ncurses shell; the
  version is assigned later by the maintainer.
- **Epic:** tracks the seven milestones below; each milestone is one PR.
- **Scope owner:** `tui/`.
- **Related:** `docs/spec/tui/architecture.md`, `docs/style/tui-design-guide.md`,
  `docs/spec/tui/layout-engine.md`, `docs/spec/tui/window-projects.md`.

## Goal

Make the ncurses client a coherent product: one component system, one theme
layer, one input router, and a shell that never looks like a collection of
screens that grew independently.

The bottom bar and prompt are already the product's densest, most refined
surface. They stay. The work is the shell around them and the component path
under every other screen.

## Locked decisions

| Area | Decision |
|---|---|
| Window model | Full-screen workspace per tab. `SplitPane` is designed into the model but not required for the shell. No overlapping desktop. |
| Tabs | Chat sessions only. Tools, panels, skills, jobs and the work board open as layers inside the active tab. |
| App mark | A bare `◆` at the far left, amber, no frame. It toggles the menu; F10 is the keyboard path. |
| Top rail | One row of capped tabs, one rule row beneath it. Classic density. Compact drops the rule. |
| Clock | Top-right, OSX style: `HH:MM`, no seconds. |
| Health pulse | A slow `●` beside the clock, bright/dim on a one-second beat. It reports that the UI loop is alive, especially over SSH. It is not an internet claim. |
| Right tray | Icons only: plugin status glyphs, failure icon only when the provider link actually fails, clock rightmost. No text labels. |
| Bottom | Unchanged status bar and prompt. The clock leaves the status bar; freed width reduces dropping. No F-key row. |
| Hints | Ghost text on the input row after the prompt, after idle. Static `hints.json`, shuffle bag, no LLM. |
| Menus | `menus.json` + `keybindings.json`, both referencing one ActionRegistry. `completions.json` remains the typed command surface. |
| Shortcuts | F10 is the only advertised app key. Ctrl+N/O and Alt+1..9 are aliases. Ctrl stays for editing. No default F2/F4/F6 dependencies. |
| Themes | Amber Classic default (blue chrome, dark navy transcript), Modern, High Contrast, Monochrome. 8/16/256-color and ASCII fallbacks. |
| Consistency gate | Only the ncurses adapter includes `<ncurses.h>`; raw pair/paint calls are ratcheted down to the theme/renderer. |

## Visual contract

```
◆  ▐ 1 amber-agent × ▌  ▐ 2 docs ● ▌  ▐ 3 infra ! ▌        ⚑  ◈  ● 12:34
═══════════════════════════════════        ══════════════════════════
  transcript
  ...
 [s1/3] [qwen|gpt-5] write  lag 0.3s  24 t/s  ↑12 ↓345  ctx ▓▓▓░░ 42%  bash…
 └─[amber]─[main]─[+12/-3]─❯ Hint: F10 opens the top menu
```

- The tab rail uses `▐ … ▌` caps. Active tab: bright filled background, bold.
  Inactive: dim. Hover: mid shade.
- The rule is `═`; it opens under the active tab so the tab connects to the
  transcript.
- Running: `◐`; done-unread: `●`; needs-input: `!`; error: `✗`.
- The right tray is icon-only. The pulse sits between the tray and the clock.
- The prompt keeps `└─[project]─[branch]─[+ins/−del]─❯`.
- The hint starts after the prompt; it is never part of the input buffer.

## Milestones

Each milestone is one PR. The epic closes when all seven are merged.

### M1 — Render foundation

Cell/Surface/Compositor, semantic theme roles, glyph set, component base,
focus/hit map, headless cell-grid tests, component gallery.

**Done when:** a component can be painted into a cell grid and asserted without
a TTY; a surface diff can be blitted to ncurses; the gallery renders every
primitive at 80×24 and 120×40 in Unicode and ASCII.

### M2 — Top rail shell

Bare `◆`, capped session tabs with active/inactive/hover states, attention
markers, overflow, `HH:MM` clock, health pulse, icon-only right tray, rule with
active-tab opening. Bottom bar untouched except clock removal; prompt untouched.
Mouse click on tabs and the app mark.

**Done when:** session tabs render and switch, the active tab is always visible,
overflow is reachable, the clock/pulse repaint without a full-screen redraw, and
the pty suite covers tab click, active/inactive rendering and narrow widths.

### M3 — Menus, shortcuts and hints

`menus.json`, keybindings v2, ActionRegistry with label/enabled/handler,
F10/mark menu mode, submenus, mnemonics, Esc unwinding, conformance tests,
`hints.json` + `HintRotator` ghost text.

**Done when:** every menu/keybinding action resolves; every action is classified
as menu, slash-only or internal; no key or hotkey collides; hints appear only
when idle/empty and disappear on the first key; the menu is fully keyboard- and
mouse-operable.

### M4 — Dialogs and modals

Confirm, info, form, list and panel dialogs on the component layer. One modal
stack for approvals, asks, API keys and plugin prompts. Toast/status notices.
Worker-blocking semantics preserved.

**Done when:** approval/ask/API-key flows are unchanged under load; a modal
opened from a menu returns focus to the tab it covered; no nested ncurses dialog
corruption.

### M5 — Screen migration

Settings and provider picker, sessions and project picker, skills explorer,
plugin panels, model picker/drawer, work board. All paint through the component
tree.

**Done when:** every user-facing screen uses the same frame, list, form and
dialog primitives; no screen calls `mvwadd*`/`newwin` directly.

### M6 — Chat surface

`TextCanvas` for `rich::Line`, cached wrapping, scroll performance, working
indicator, markdown/theme integration. Final ncurses include ratchet.

**Done when:** 10k lines scroll within the frame budget, resize rewraps once,
and the only file including `<ncurses.h>` is the terminal adapter.

### M7 — Shell hardening

Amber Classic, Modern, High Contrast and Monochrome palettes; ASCII and
8/16/256-color fallbacks; no-color-only attention; reduced motion; terminal
matrix; performance budgets; docs; release notes.

**Done when:** the pty suite passes on xterm, tmux and a small terminal; the
terminal matrix and key caveats are documented; the shell gates below pass.

## Shell release gates

- `make verify-quick` and `make verify` clean; no new clang-tidy or cppcheck
  findings.
- `make test` green, including pty tests, under both compilers in CI.
- Only the terminal adapter includes ncurses; the ratchet has no exceptions.
- Every user-facing screen paints through the component tree.
- Menu/keybinding/action conformance: zero unresolved actions, zero collisions.
- Component cell-grid tests at 80×24 and 120×40, Unicode and ASCII.
- pty tests: tab click/close/overflow, attention, background approval, menu
  open/close, hint show/hide, clock/pulse tick, terminal-too-small.
- No flicker on the clock tick; no full-screen repaint for the pulse; input
  latency under 50 ms.
- Accessibility: high contrast, monochrome, ASCII, no blink, reduced motion.
- Session restore preserves tab order, active tab and scroll position.

## Non-goals for this shell

- Overlapping/draggable windows.
- A second command surface beside `completions.json` + action IDs.
- A prettier prompt: the prompt stays as it is.
- Replacing the bottom status bar's information design.
- LLM-generated hints.

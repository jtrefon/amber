# Plan: TUI 1.0 beta gates

- **Status:** gate list for the beta; each gate is a command or a manual check,
  not a claim. `#519` owns the shell half.
- **Related:** `docs/plan/tui-shell.md` (milestones M1–M7),
  `docs/spec/tui/terminal-matrix.md`, `docs/spec/tui/themes.md`.

## Automated gates

| Gate | Command | Passes when |
|---|---|---|
| Static | `make verify-quick` | clean |
| Build + analyzers + tests | `make verify` | clean, zero clang-tidy/cppcheck findings |
| Both compilers | CI `build-and-test (g++)` / `(clang++)`, `WERROR=1` | green |
| Sanitizers | CI `sanitizers`, `tsan` | green |
| Coverage | `make coverage BASE=origin/main` | project ≥80%, TUI floor, patch ≥80% |
| Terminal matrix | `make pty-matrix` | xterm pty suite green; 58×17 included. tmux dispatch is a measured gap under #519. |
| Gallery | unit `gallery_renders_every_primitive_*` | every palette × glyph mode at 80×24 and 120×40 |
| `NO_COLOR` | pty `no_color_still_paints_a_usable_frame` | paints, no 256-colour sequence |
| Ratchet | `make ncurses` | one file: `tui/terminal.cpp` |

## Manual gates

- tmux/screen session: open the gallery and a chat, resize, scroll, switch a
  theme; no stale rows or flicker.
- macOS Terminal and iTerm2: Option digit row and Option+B; F10 menu.
- Linux console: ASCII glyphs, 8 colours, prompt and box drawing legible.
- kitty/alacritty/wezterm: mouse wheel, bracketed paste, palette switch.
- High-contrast and monochrome palettes: every attention mark is visible without
  relying on hue.
- Reduced motion: spinner, gauge sweep and clock colon are static.

## Release notes

- `docs/keybindings-v2-migration.md` for users carrying a v1 keybindings file.
- `CHANGELOG` is not used; the GitHub release notes carry the feature summary
  (v0.8.8 includes the shell migration through the model drawer and chat
  surface).

## Not a gate

A hermetic benchmark cannot measure a palette or a prompt's feel; those stay
manual. A green unit suite says the cells are right, not that the terminal is
legible.

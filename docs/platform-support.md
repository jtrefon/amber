# Platform and terminal support (0.9)

Status: the support matrix for the 0.9 production pre-release.
Applies to: `amber` (TUI), `amber-cli` and `amber-bench`.
Rendering reference: `docs/terminal-compatibility.md` explains the
ncurses/ACS/Unicode paths; this page says what is supported and how it is
tested.

## How to read this page

- **Gated** — a CI job fails when the cell breaks. The job or command is named.
- **Manual** — a person runs the procedure below in the named terminal and
  records the result in #698 before the 0.9 tag. A manual cell with no recorded
  run is **not** part of the 0.9 claim.
- **Not supported** — amber is not claimed to work there. It may build; that is
  not support.

A terminal that is not listed is not supported. Silence is not a support claim.

## Operating systems

| OS / architecture | Status | Evidence |
|---|---|---|
| Linux x86_64 (glibc; Ubuntu 24.04) | Gated | `build-and-test (g++)`, `build-and-test (clang++)`, `tui-agent-e2e`, `agent-e2e`, `sanitizers`, `tsan`, `coverage`, `static-gates` on `ubuntu-24.04`. Release artifact `amber-<version>-linux-x86_64`. |
| macOS arm64 (Apple Silicon) | Gated | `build-and-test-macos` on `macos-latest`: `make`, `make test`, `tests/tmux_smoke.sh ./amber`, `./tui_pty_test ./amber --repeat 2`, `make tui-agent-e2e REPEAT=2`. Release artifact `amber-<version>-darwin-arm64`. |
| macOS x86_64 (Intel) | Not supported | README: Apple Silicon only; no artifact and no runner. |
| Linux arm64 / aarch64 | Not supported | No release artifact and no gate; a source build is not a support claim. |
| Windows native | Not supported | No build and no terminal adapter. |
| WSL2 (Ubuntu on Windows) | Manual | The Ubuntu package install is documented; run the manual procedure in Windows Terminal and record it. |

## Terminals and multiplexers

| Terminal / multiplexer | `TERM` | Status | Evidence / steps |
|---|---|---|---|
| bare pty (journey harness) | `xterm-256color` | Gated | `make tui-agent-e2e` (27 journeys) and `./tui_pty_test ./amber`; the harness sets 80x24, and tests that need another size ask for it. |
| tmux | `screen-256color` / `tmux-256color` | Gated (macOS and Linux) | `tests/tmux_smoke.sh ./amber` in `build-and-test-macos`; `make terminal-smoke` in the Linux `tui-agent-e2e` job. Locally `make terminal-smoke` or `./tests/tmux_smoke.sh ./amber`. |
| Terminal.app (macOS) | `xterm-256color` | Manual | Procedure below; record in #698. |
| iTerm2 (macOS) | `xterm-256color` | Manual | Procedure below; also check Option/Alt mapping and mouse reporting. |
| GNU screen | `screen-256color` | Gated (Linux) | `tests/screen_smoke.sh ./amber` via `make terminal-smoke` in the Linux `tui-agent-e2e` job; locally `make terminal-smoke`. Slash commands only; Alt chords, paste and the mouse remain manual. |
| plain ssh | client's `TERM` (forwarded) | Manual | Procedure below; forward a UTF-8 locale (`SendEnv LANG LC_*` or `ssh -t`). |
| other xterm-256color emulators (Alacritty, kitty, WezTerm, GNOME Terminal, Konsole) | `xterm-256color` | Manual | Procedure below. |
| PuTTY | `xterm` / `putty` | Manual, not claimed by default | Rendering may need `NCURSES_NO_UTF8_ACS=1`; see `docs/terminal-compatibility.md`. |
| Linux console | `linux` | Not supported | No UTF-8/256-color; use a terminal emulator. |
| non-UTF-8 locale or 8/16-color `TERM` | any | Not supported | The pty suite runs UTF-8 and 256 colors; `set display glyphs ascii` is a rendering fallback, not a support claim. |

## What the automated suite covers

- `tui_pty_test` pins the interface: keys, dialogs, drawers, redraws, the quit
  path and the exit-ready marker, at 80x24 and at the larger sizes a test asks
  for.
- `tui_agent_e2e_test` pins the workflows: 27 journeys through `main()`, the
  plugin runtime, real tools and a loopback endpoint on `xterm-256color`.
- `tests/tmux_smoke.sh` drives the real binary in a real tmux pane
  (`screen-256color`) and asserts the welcome prompt and a typed `/window list`.
- `tests/screen_smoke.sh` does the same in GNU screen (`screen-256color`):
  startup paints and `/window list` is dispatched. `make terminal-smoke` runs
  both; neither smoke needs an inference endpoint.
- The CLI journeys (`bench/scenarios/e2e/`) and the unit suite do not depend on
  a terminal emulator.

**Not covered automatically:** a real emulator's font and glyph coverage, OS
clipboard integration, IME, terminal-specific Option/Alt mapping, tmux/screen
key interception, SSH locale forwarding, window-manager resize behaviour and
accessibility. Those are the manual cells.

## Manual procedure (one run per cell)

1. Build or install the 0.9 candidate on the OS in the table.
2. Record the environment: `amber --version`, OS/arch, terminal name and
   version, `$TERM`, `locale`, and whether tmux/screen/ssh is in front.
3. Start `amber` in a scratch project with a configured provider (or the local
   endpoint). Do not use a real project for the first run.
4. Check:
   - startup paints the welcome/top rail and the workspace in the status line;
   - a prompt runs a turn; a tool call renders and the answer arrives;
   - `/help`, `/usage` and `/get config paths` render, and the drawer completes;
   - `/window list`, open a second tab (Ctrl+N), switch by Alt+1/2 and by
     clicking the tab, then close it;
   - resize the window or pane; the layout stays usable;
   - paste a multi-line block; the composer keeps it (or flattens it per
     `/set input multiline`);
   - the mouse wheel scrolls the transcript; a click selects a tab;
   - approve and deny a bash command; the dialog keys work;
   - `/quit` exits through the normal path, without a kill.
5. Record pass/fail with a screenshot or transcript in #698. A failure is a bug:
   fix it, or mark the cell unsupported for 0.9.
6. Repeat in the same cell after the fix; a stale result is not evidence.

## Known gaps and caveats

- **#621 (macOS tab-switch freeze).** Closed on 2026-11-25 as a recorded 0.9
  deferral. The `tab-switch-live` journey covers the switch path on a pty, and
  the dispatch-only `tui-freeze-probe.yml` repeats it with a macOS stack
  sample (first run 20/20); the root cause is carried into 1.0 (#562). The
  macOS tab-switch cell remains part of the manual procedure, and the freeze
  is recorded as `known_unmeasured` in the flake budget (#705).
- **tmux and screen key interception.** `Alt+1..9`, `Esc` chords and the mouse
  can be claimed by the multiplexer; the manual run must say whether the keys
  reach amber or the multiplexer.
- **SSH.** The client's `TERM` and locale are forwarded, not chosen by amber; a
  non-UTF-8 locale is not supported.
- **WSL2.** The package install is documented, but the terminal path (Windows
  Terminal -> WSL2 -> pty) is manual.
- **Intel macOS, Linux arm64, native Windows.** No release artifact; a source
  build is not support.
- **The pty is not a terminal emulator.** A green pty run says the ncurses
  contract works; it does not prove a specific emulator renders it.

## Recording the 0.9 support claim

The 0.9 release checklist requires a recorded result for every **Manual** cell
the release claims: Terminal.app, iTerm2, plain ssh, WSL2, PuTTY and any other
xterm-256color emulator named in the release notes. tmux and GNU screen are
gated by `make terminal-smoke` on Linux and by the macOS tmux smoke. The record lives in
#698 with the environment and the result. If a cell is not recorded, the release
notes name it as unverified; it is not claimed.

## Adding a terminal

Add a row with a status and the evidence. A gated row needs a CI step that runs
the named command; a manual row needs the procedure above and a recorded run. Do
not add a row for a terminal nobody has exercised.

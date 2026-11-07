# Spec: terminal matrix and key caveats

- **Status:** 0.8. `#519` shell hardening.
- **Related:** `themes.md`, `keybindings.md`, `docs/style/tui-design-guide.md`.

## What is tested

The pty suite drives the real binary through a pseudoterminal. The same suite
runs with `AMBER_PTY_TERM` selecting the terminal type:

```sh
make pty-matrix
# the full pty suite on xterm-256color, plus a real tmux pane smoke
```

The suite already includes a 58×17 list-only layout and 120×40/80×24 layouts.
`NO_COLOR=1` is a pty scenario; the gallery renders every palette in Unicode and
ASCII.

## Matrix

| Terminal / environment | Status | Notes |
|---|---|---|
| xterm / xterm-256color | primary | full colour, UTF-8, mouse, bracketed paste |
| tmux / screen | supported | the tmux smoke opens a real pane, types `/window list` and asserts the output; the welcome-window transition is covered by a pty test |
| Linux console | supported, ASCII-safe | 8 colours; `DarkGray` falls back to white; use `/set display glyphs ascii` if the font lacks box drawing |
| macOS Terminal / iTerm2 | supported | Option-as-text is decoded for the Option digit row and Option+B; `Alt` sequences otherwise need the terminal's "Esc+" setting |
| kitty / alacritty / wezterm | supported | xterm-256color behaviour; truecolor is not required |
| `NO_COLOR` | supported | attributes only; no pair registration |

## Key caveats

- **Bare Ctrl:** some terminals intercept Ctrl+Tab/Ctrl+Shift+* before the app;
  the shipped bindings use Ctrl+letter forms that pass through a raw pty.
- **Ctrl+Tab:** not portable as a distinct key. Use `Alt+1..9` or the tab rail
  click instead; a window manager may keep the chord.
- **Alt+letter:** sends an ESC prefix, or a literal Option glyph on macOS with
  default settings. The input path decodes both; a terminal that swallows Alt
  entirely cannot be helped by the app.
- **F-keys:** F10 is the menu key in the shipped table. Some terminal profiles
  map F10 (or F1–F5) to their own menus; `keybindings.json` can remap it.
- **Bracketed paste:** needed for multi-line/secret pastes; terminals without it
  deliver the bytes as keys, which the input line treats the same way minus the
  paste framing.
- **Mouse:** reporting is enabled at startup and toggled with `Alt+M`; enabling
  it replaces terminal drag-select, which is the documented trade-off for wheel
  scrolling and tab clicks.

## Flicker and budget

`Terminal::blit` writes the virtual screen once and `Terminal::flush` calls one
`doupdate()`. A frame is not refreshed per widget. The 10k-line scroll test
asserts one wrap and a bound well inside the frame budget; the canvas repaint
writes only its viewport.

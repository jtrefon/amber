# Spec: themes, glyphs and motion

- **Status:** 0.8. `#519` shell hardening.
- **Related:** `render-foundation.md`, `chat-surface.md`, `terminal-matrix.md`.

## Palettes

`Theme` maps semantic `Role`s to colour and emphasis. A screen never names a
colour, so a palette swap changes no paint code.

| Palette | Intent | Key choices |
|---|---|---|
| **Amber** (default) | warm CRT | cyan frame/title, yellow accent/selection, green success, red error |
| **Modern** | cool, high-chroma | blue frame, magenta accent/attention, white selection on blue |
| **High contrast** | low vision, bright terminals | white bold frame/title, black-on-white selection, reverse attention |
| **Monochrome** | no colour, screen readers, dumb terminals | default fg/bg only; distinction is bold, dim and reverse |

`Theme::make(ThemeKind, GlyphMode)` is the only constructor; `Theme::amber()` is
the default wrapper. Palettes are unit-tested for the properties that matter:
monochrome has no fixed colours and uses attributes, high contrast separates
selected fg/bg, and every palette keeps the same semantic role structure.

## Selection and persistence

| Setting | Values | Effect |
|---|---|---|
| `/set display theme` | `amber`, `modern`, `high-contrast`, `monochrome` | re-registers terminal pairs and repaints immediately |
| `/set display glyphs` | `auto`, `unicode`, `ascii` | switches the global glyph vocabulary (`text::glyph`) |
| `/set display motion` | `full`, `reduced` | freezes the spinner, gauge sweep and clock colon |

They persist in the project settings file (`ui_theme`, `ui_glyphs`,
`ui_motion`) and are reported by `/get display`. The command tree owns the
choices (`completions.json`), so the drawer and help share them.

`auto` glyphs follows the locale and `AMBER_ASCII`; `unicode` and `ascii` force
the mode for the current process. The override lives beside
`text::glyph::utf8()`, so every painter that already asks the locale question
follows it without a second path.

## Colour depth and `NO_COLOR`

`Terminal` resolves a role to a pair at blit time:

- **256 / 16 colours:** base ANSI roles; the welcome raster uses xterm's
  232–255 grayscale ramp.
- **8 colours:** `DarkGray` falls back to white, so a role never disappears.
- **No colour support or `NO_COLOR` non-empty:** no `start_color`, no pair
  registration, attributes only. A 256-colour sequence is asserted absent in
  the pty suite.
- **ASCII:** box drawing and state marks use the ASCII glyph set; no double-width
  glyph is emitted.

Attention is never colour-only: tabs and the tray carry a glyph (`!`, `●`, `✗`)
and the high-contrast/monochrome palettes use bold/reverse as well as hue.
There is no blink anywhere.

## Gallery

`amber --gallery` is the visual acceptance surface. `a` swaps Unicode/ASCII and
`t` cycles Amber → Modern → High contrast → Monochrome; the footer names the
active palette. The pure gallery test renders every (palette, glyph mode) at
80×24 and 120×40 and asserts the primitive structure is identical across
palettes (only the colour resolution differs).

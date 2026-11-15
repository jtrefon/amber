# Spec: themes, glyphs and motion

- **Status:** 0.8. `#519` shell hardening.
- **Related:** `render-foundation.md`, `chat-surface.md`, `terminal-matrix.md`.

## Palettes

`Theme` maps semantic `Role`s to colour and emphasis. A screen never names a
colour, so a palette swap changes no paint code.

| Palette | Intent | Key choices |
|---|---|---|
| **Amber** (default) | warm CRT, commander chrome | blue sheet and bar, cyan frame, bold white title, yellow accent, black-on-cyan selection, green success, red error |
| **Modern** | cool, high-chroma | blue sheet and bar, cyan/white frame, magenta accent/attention, black-on-cyan selection |
| **High contrast** | low vision, bright terminals | white sheet with black text, black bold frame/title, reverse focus, no hue-only signals |
| **Monochrome** | no colour, screen readers, dumb terminals | default fg/bg only; distinction is bold, dim and reverse |

`Theme::make(ThemeKind, GlyphMode)` is the only constructor; `Theme::amber()` is
the default wrapper. Palettes are unit-tested for the properties that matter:
monochrome has no fixed colours and uses attributes, high contrast separates
selected fg/bg, and every palette keeps the same semantic role structure.

The chrome roles are separate from the transcript roles because they carry a
background: `Banner` paints the top and bottom bars (white on blue in Amber and
Modern), `Brand` is the amber mark on that bar, `Gauge*` are the pressure colours
and glint on the bar, and `ShineMid`/`ShineHi` are the verb's two fade steps on
the transcript background. A palette may change the hues, but it keeps those
roles distinct; collapsing them is how the 0.8.9 theme migration lost the blue
bar and flattened the pulse.

## Modal sheet roles

A modal is one blue surface, not a gray window with text punched through it.
The sheet roles carry the surface background, so body text and labels never
paint the terminal default behind themselves:

| Role | Amber / Modern | High contrast | Monochrome |
|---|---|---|---|
| `DialogSurface` | white on blue | black on white | default |
| `DialogText` | white on blue | black on white | default |
| `DialogMuted` | cyan on blue | black on white, dim | default, dim |
| `DialogTitle` | white on blue, bold | black on white, bold | default, bold |
| `DialogFrame` | cyan on blue, bold | black on white, bold | default, bold |
| `DialogAccent` | yellow on blue | black on white, bold | default, bold |
| `DialogSelected` | black on cyan | white on black | default, reverse |
| `DialogButton` | black on cyan | black on white | default, reverse |
| `DialogButtonFocus` | black on white | white on black | default, reverse, bold |
| `DialogFooterKey` | black on cyan | black on white, bold | default, reverse |
| `DialogFooterLabel` | cyan on blue | black on white | default |
| `DialogError` | white on red | black on white, bold underline | default, reverse, bold |
| `Shadow` | dark gray on default, `░` | black on default, `░` | default, dim, `░` |

The painter never uses the transcript `Text`/`Muted` roles inside a sheet: those
carry the terminal default background and leave black holes in the blue. The
`Shadow` role carries a glyph (`░`, or `:` in ASCII) because a black-on-black
block is not a shadow on a dark terminal. `P_DIALOG` keeps resolving to
`Banner` for legacy pair consumers; the modal roles are the contract for
`dialog_view`.

The full visual contract is `dialogs.md` §4.

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

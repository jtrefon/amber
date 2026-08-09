# Amber Website Plan

## Core Philosophy

The site should feel like amber itself - terminal-native, IRC-adjacent, no corporate AI gloss.
Think: a well-maintained GitHub README that expanded into a full site, not a SaaS landing page.
The nostalgia leaks through in the details, not as a gimmick.

## Framework: Astro

- **Why Astro over vanilla HTML:** Built-in SEO (metadata, sitemaps, Open Graph), minification,
  image optimization, structured data. Ships zero JS by default (islands architecture).
  Can use vanilla HTML/CSS for terminal aesthetic, React/Svelte only for interactive islands.
- **Deployment:** GitHub Pages (Astro has a one-command adapter).
- **Performance:** The site should load in under 1 second. No external CDN calls.
  Self-host fonts (JetBrains Mono).

## Aesthetic Direction

### Primary Palette — amber CRT phosphor (2026-08-09 revision)

The aesthetic is now driven by the amber monochrome monitors of the P3-phosphor
era (IBM 5151, ZX Spectrum, the BBS/IRC nights) — the name "amber" is the old
monitor, not the modern orange hex. Black glass, amber text, box-drawn frames,
scanlines, phosphor glow:

- Background: `#000000` (pure black — matches the art.ans robot's black canvas)
- Bright phosphor: `#ffb000` (primary accent, headings, links, cursor)
- Hot phosphor: `#ffcf4d` (hovers, h2 titles, status dot)
- Text: `#e8cf9c` (warm phosphor white)
- Dim text: `#9a7c47`
- Faint: `#5c4823`
- Frame lines: `#33260e` / `#6b4f1f` (borders, dashes, `┌─` bars)
- Monospace fonts throughout: JetBrains Mono (self-hosted)
- No rounded corners. No gradients (flat CRT wash only). No stock photos.
- CRT details: scanline overlay (`body::after`), phosphor text-shadow glow,
  blinking `█` cursor, dashed rule dividers with a glowing `◆`, box-drawn
  `┌─ title ─┐` section frames.

### The "Leak" — Where Nostalgia Bleeds Through

1. **ASCII art hero section.** The `art.ans` file (44KB) is your hero. Render it as a scrolling/
   animated ASCII banner at the top. Not a screenshot - the actual ASCII art.

2. **ANSI-colored text blocks.** Render terminal-colored text using `ansi-to-html` or similar.
   The `/set` commands, the status bar, the tool calls - they look like they're from amber itself.

3. **Terminal-style navigation.** Nav bar styled as a command prompt:
   `amber ~/website $ [Home] [Docs] [Benchmarks] [Install]`
   Typing `/` opens the drawer. Micro-interaction that says "this is amber."

4. **Status bar at the bottom.** Like the amber TUI. Connection state indicator, version number,
   current section. Static but present.

5. **ASCII dividers between sections:**
   ```
   ──────────────────────────────────────────
   │ amber: an AI agent harness for Linux   │
   ──────────────────────────────────────────
   ```

6. **Blinking cursor.** A `█` cursor on the hero text. Simple, right detail.

7. **Terminal window mockups.** Real terminal captures of amber running - tool calls, status bar,
   the whole thing. Not stylized screenshots, actual terminal renders.

### What to Avoid
- Gradients, rounded cards, hero images of people, stock photos
- "AI-powered" language
- SaaS clichés ("Join our community", "Experience the power")
- Any corporate gloss

## Site Structure

### 1. Hero / Landing

The ASCII art banner. Below it, one line:

> "amber: an AI agent harness for Linux servers"

Then the pitch - three lines max:

> Built in C++. Bare-metal performance. Open source (Apache 2.0).
> A terminal-native agent runtime with a BitchX-inspired TUI, real
> benchmarking, and a compression pipeline that actually works.

Then a terminal window showing amber running - a real terminal capture, not a mockup.
This is your "what is this" moment.

**CTA:** `git clone https://github.com/jtrefon/amber && cd amber && make && ./amber`

### 2. "Why C++?" / The Architecture

Terminal-style architecture dump:

```
┌─ libagent_core.a ──────────────────────────────────────────────┐
│  Agent loop · LLM client · Tool registry · Compression pipeline │
│  Context integrity (FNV-1a hash chain) · MCP transport         │
└───────────────────────────────────────────────────────────────┘
         │              │              │
    ┌────▼─────┐  ┌────▼─────┐  ┌────▼─────┐
    │ amber    │  │amber-cli │  │amber-bench│
    │ (TUI)    │  │(headless)│  │(KPI harness)│
    └──────────┘  └──────────┘  └──────────┘
```

Key points:
- **Bare-metal C++17.** Not a Python wrapper. Not a Node.js script. Compiled C++ with libcurl,
  ncurses, zero Python dependency.
- **Compression pipeline.** Classify-then-extract with memory/skill ops. "900/1000 on
  Qwen3.6-27B" is your number.
- **Security model.** Workspace confinement, approval gating, process isolation.
- **Real benchmarking.** The `amber-bench` system with KPI aggregation. Tested against
  Qwen, Nemotron, Laguna, Gemma - show the data.

### 3. Benchmark Results

Terminal-style table:

```
Model                    Score   Pass  Tools  Redun  Steps
qwopus-27b               900    24/25  109     17    121
Qwen3.6-27B dense        910    24/25  104     10    115
Qwen3.6-27B MTP          901    24/25  104     11    117
gemma4-31b               903    24/25  103      4    127
Qwen3.6-35B MoE (A3B)    877    23/25  137     16    145
NVIDIA Nemotron 550B     806    21/25  133     15    149
```

This section should feel like a terminal, not a chart. The data speaks for itself.

### 4. Installation / Quick Start

Terminal-style, step-by-step:

```bash
# Install dependencies
sudo apt install build-essential libcurl4-openssl-dev libncurses-dev

# Clone and build
git clone https://github.com/jtrefon/amber
cd amber
make

# Run
./amber
```

Then "what happens next" - the welcome screen, the command drawer, the `/set` commands.

### 5. The TUI (BitchX-inspired)

Personality section. Show the TUI - the status bar, the command drawer, the tool calls,
the compression status.

Key features:
- IRC-style multi-window chat (like BitchX)
- Slash commands (`/set model`, `/get model list`, `/set provider`)
- Tool call visibility - see what the agent is doing, not just what it says
- Compression status - live progress during compression
- Approval dialogs - the agent asks before running shell commands

### 6. Technical Deep Dive

For people who want the architecture:
- Context integrity - the FNV-1a hash chain
- Compression pipeline - classify-then-extract with memory/skill ops
- Tool registry - the `ToolRegistry` with approval gating
- Security model - workspace confinement, process isolation
- MCP integration - full MCP support with stdio and HTTP transports
- Sub-agent architecture - parallel/serial mode with cache optimization

### 7. Roadmap / Future

What's coming:
- Long-duration research projects (tmux, days/weeks)
- More search backends (semantic search with real embedding models)
- Plugin system expansion
- Cross-platform considerations

## Copy Tone

**Genuine, not hype.** The tone should be: "this is a real tool built by someone who knows
what they're doing, and here's the data to prove it."

Bad: "Revolutionary AI agent harness that changes everything!"
Good: "Built in C++. Bare-metal performance. Open source."

Bad: "Experience the power of amber today!"
Good: "git clone && make && ./amber"

Bad: "Join our community of 10,000+ users"
Good: "Apache 2.0. Fork it, modify it, make it yours."

The voice: confident but not arrogant, technical but not dry, nostalgic but not gimmicky.

## The "Hook" — What Makes People Stay

The hero section delivers three things in order:
1. **ASCII art** - unusual, catches the eye
2. **Terminal window** - shows what amber actually does
3. **The install command** - the CTA, and it's the same command you'd type on your machine

The visitor should understand what amber is, why it's different, and how to try it in under 10 seconds.

## Domain

`amber-agent.org` or `amber-agent.dev` - ties to the project name without conflicting
with "amber" the general term. (Currently deployed on GitHub Pages at
`jtrefon.github.io/amber`; custom domain is aspirational — `astro.config.mjs`,
OG tags and JSON-LD already reference `amber-agent.dev`.)

## Decisions (2026-08-09)

Positioning and mechanics locked in review:

- **Claim the lane, not the throne.** No "replaces Claude Code/Codex/Aider"
  language on the site. Homepage pitch: *"The only C++17 agent harness in the
  field — compiled, not scripted. One binary, no Python, no Node, no accounts,
  no telemetry. A BitchX-inspired TUI, real benchmarks, compression that works."*
  The "replacement" framing stays in `docs/spec/VISION.md` as far-term vision.
- **Nostalgia: name it once, show it always.** One BitchX/IRC mention in the
  homepage pitch; everything else is carried by the visuals (drawer, status bar,
  aliases, `Alt+1..9` window switching — the real keybind, per `tui/tui.cpp`).
- **Mission page** is the web distillation of `docs/spec/VISION.md` +
  `MISSION.md`: north star ("AI on Linux, unleashed"), mission, spirit, who it's
  for (devs, sysadmins, security, researchers — "for everyone, without
  discrimination"), and the we-are/we-are-not table.
- **Roadmap is bound by the mission.** Windows/macOS, web/GUI, plugin loaders,
  SaaS/telemetry, mobile are "Not on the Roadmap" (they were previously listed
  as future work, contradicting the mission's feature filter).
- **Repo URL is `github.com/jtrefon/amber`** — never `trefon/amber` (404).
- **Benchmarks**: homepage shows top 3 rows + link; the benchmarks page is the
  full 9-model table. "31 scenarios, 6 suites" is the count everywhere.
- **ASCII hero** is rendered inline as static HTML (`website/src/data/ascii-hero.html`,
  generated from `art.ans` by the 256-color converter, injected via `?raw` +
  `set:html`), not an iframe. No JS.
- **Fonts are self-hosted** (`/fonts/jetbrains-mono-*.woff2` from @fontsource) —
  no Google Fonts CDN, per the <1s load promise.
- **Base path** (`site: 'https://jtrefon.github.io/amber'` in `astro.config.mjs`)
  — project Pages serve under `/amber/`, so Astro emits `/amber/_astro/...` asset
  URLs. Without it every stylesheet 404s and the site renders as bare HTML
  (the bug that shipped the unstyled site). OG tags point at the live Pages URL;
  flip to `amber-agent.dev` when the custom domain lands.
- **Download page** (`/download`) — binaries first, source second. Direct links to
  the v0.3.1 release assets (.deb / .rpm / tarball + checksums + CDP plugin).
  The homepage "Get amber" section leads with `dpkg -i`/`rpm -i`; building from
  source is the fallback, never the gate. Bump the version strings with each release.
  Facts to keep straight (verified against the v0.3.1 tarball):
  - The tarball is a staged install tree (binaries + libs + headers + data),
    extracted to `/` with `sudo tar -xzf ... -C /`. It does NOT run from an
    arbitrary location: the binaries need the data files at the install prefix.
  - The packages ship `amber`, `amber-cli`, `libagent_core.a`/`libagent_tools.a`
    and headers. `amber-bench` is dev-only and is not installed.
- **Arch Linux** — supported via `packaging/arch/PKGBUILD` (source build from the
  release tag, `check()` runs the unit suite, sha256 pinned). Not on the AUR.
  Download page card: `curl -O ...PKGBUILD && makepkg -si`.
- **Footer is the status bar, nothing else.** The fixed status bar is the footer;
  there is no scrolling `<footer>` element (a second footer was added once and
  removed). Creator credit lives in the status bar right side:
  `Jacek Trefon www.trefon.com` (links to trefon.com). No email addresses on the
  site; no mailto links.
- **Manual** (`/manual`) — rebuilt from the real command tree (`completions.json`,
  the single source of truth): model/provider, sessions/jobs, policy, files/system,
  MCP/plugins, skills/engine, windows, config, env vars. Never hand-write commands
  that contradict the tree.
- **One harness, four parts** — homepage section promoting `libagent_core.a`
  (portable library), `amber-cli` (headless/workflow), `amber-bench` (KPI harness),
  `amber` (TUI). The library's embeddability is a first-class selling point.
- **No emdashes** — "—" is banned from rendered copy (SEO hygiene); use commas,
  colons, or hyphens. `rg "—" website/src` must return nothing.
- **SEO baseline**: robots.txt (Allow all + sitemap), per-page canonical tags,
  h1 on the homepage hero, meta descriptions everywhere. Lighthouse (live):
  SEO 100, Performance 98, Accessibility 91. Site is not yet indexed (DuckDuckGo
  "no results", Aug 2026); submit to Google Search Console + Bing Webmaster Tools
  after the custom domain is live.

## Interactive Islands (Astro + React/Svelte)

Only a few interactive elements need JS - the rest is static:

1. **Terminal cursor blink** — blinking `█` on hero text (tiny React island)
2. **Scroll-triggered ASCII animation** — ASCII art shifts as you scroll (IntersectionObserver + CSS)
3. **Nav drawer** — typing `/` opens the command drawer (React island)
4. **Terminal window mockup** — fake terminal that types itself out (React island)
5. **ASCII art hero** — the `art.ans` rendered as a terminal window (React island)

Everything else: static HTML/CSS. Zero JS shipped by default.

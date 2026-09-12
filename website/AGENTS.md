# AGENTS.md, amber website (Astro)

Static Astro site for the amber project, deployed to GitHub Pages under
`/amber/` (the `base` is rewritten post-build by `scripts/add-base.mjs`).

## Build & verify

- `npm run dev`, local dev server.
- `npm run build`, `astro build` then the base-path rewrite. Must complete
  clean; it is the gate CI runs.
- There is no test suite. Verify diagram changes in a real browser: the
  diagrams render client-side, so a passing build does not prove they render.
  Note that `global.css` sets `scroll-behavior: smooth`, so a verification
  script must force `scrollBehavior = 'auto'` before sweeping the page or the
  lazy diagrams below the fold will never enter the viewport.

## Diagrams

Architecture diagrams are authored as **mermaid source in TypeScript**, not as
ASCII art in a `<pre>` and not as images.

- **Source of truth:** `src/data/diagrams.ts`, the diagram source and its
  caption. Add a named entry there.
- **Render:** `<Diagram name="..." />` (`src/components/Diagram.astro`). Pages
  never paste diagram markup, and never pass a caption: it resolves from the
  catalog, so a diagram is described in exactly one place.
- **Gallery:** `/architecture/diagrams` renders every entry from
  `diagramCatalog`. The page throws at build time if a diagram in `diagrams` is
  missing from the catalog, so the two cannot drift.
- **Theme:** `src/diagrams/theme.ts` holds the amber CRT `themeVariables`.
  Colours mirror the tokens in `src/styles/global.css`, keep them in sync.
- **Framing:** `.diagram` in `global.css` is the figure frame. Do not wrap a
  diagram in `.terminal`: that injects the `amber ~ $` prompt, which is wrong
  for anything that is not a terminal session.

### Rules

- Mermaid is imported **lazily** (`import('mermaid')` on
  `IntersectionObserver`). Keep it that way: a static import puts ~165 KB gzip
  of mermaid core in the eager bundle of every diagram page.
- Rendering is **serialized** through one queue in `theme.ts`. `mermaid.run` is
  not safe to call concurrently, and IntersectionObserver fires several batches
  during a scroll; concurrent calls make rendering flaky.
- Only add mermaid to pages that actually have a diagram; pages without a
  `<Diagram>` must stay zero-JS.
- Keep ASCII/`<pre>` for things that genuinely are terminal output, the TUI
  captures, file trees, JSON-RPC traces, command references. Diagrams that
  describe structure or flow are vector.
- A failed diagram falls back to showing its source (`.mermaid-error`); do not
  remove that path.

### Known mermaid caveats

- **`direction` in a subgraph** is ignored when the subgraph has edges crossing
  its boundary. Do not rely on it to compact a layer; restructure the graph.
- **Angle brackets in a label** are swallowed as HTML tags, so
  `plugin.<id>.<path>` renders as `plugin..`. Use `{id}`-style placeholders.
  (In page prose the same text is fine: Astro escapes it.)
- **A semicolon in a label is a statement terminator.** In a sequence diagram it
  truncates the statement and fails the parse; in a flowchart label it survives
  but reads as a sentence break. Never use `;` inside a mermaid source.
- **Em dashes and other non-ASCII punctuation** are fine in mermaid, but confirm
  what actually landed in the file (`python3 -c "print(open(p).read().count(chr(0x2014)))"`)
  rather than trusting the editor: some toolchains transliterate `, ` into
  `;`, `:`, or `,`, which silently changes wording and can break the parse.

### Diagram inventory

25 diagrams, grouped as the gallery groups them: runtime and plugins
(`runtimeOverview`, `buildLayering`, `pluginLifecycle`, `capabilityLedger`,
`pluginTierChoice`, `pluginCommandBinding`, `externalPluginHandshake`), the
command system (`commandTreeStructure`, `commandTreeDispatch`, `commandFeeds`),
the engine (`agentLoopCycle`, `toolDispatch`, `compressionPipeline`,
`contextHashChain`, `promptAssembly`, `errorRecovery`), wire paths
(`llmDialectPath`, `mcpSessionLifecycle`), safety (`approvalGate`,
`pathConfinement`) and other subsystems (`subagentExecution`,
`searchBackendSelection`, `memoryExtraction`, `benchHarness`, `testLayers`).

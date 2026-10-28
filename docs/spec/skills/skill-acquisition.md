## Spec: Skill Acquisition (Explorer, Search, Install)

### Purpose

Define how a user finds, judges, installs, updates and removes third-party
skills from inside amber, without leaving the terminal. The acquisition surface
is the missing third leg of the skills capability: `skill-files.md` defines what
a skill *is*, `skill-catalog.md` defines what amber *knows* once a skill is on
disk, and this spec defines how a skill *gets there* and how the user decides
it should.

The design problem is not download. It is **trust under time pressure**: a
`SKILL.md` is instructions the model will follow, so the UI must make the
decision cheap (dense rows, instant search), the evidence visible (source,
audits, body preview, token cost), and the consequences explicit (scope, name
collision, discovery cost) — without turning installation into a wizard.

### Ownership

- **Source files** (target):
  - `include/agent/skill_index.h`, `lib/skill_index.cpp` — index port + value types
  - `lib/skill_index_skills_sh.cpp`, `lib/skill_index_github.cpp` — adapters
  - `include/agent/skill_ledger.h`, `lib/skill_ledger.cpp` — install provenance
  - `lib/skill_install.cpp` — tree install, digest, atomic write
  - `tui/skills_browse_core.{h,cpp}` — L1 pure state machine
  - `tui/skills_service.{h,cpp}` — L2 requests, debounce, cancellation
  - `tui/skills_explorer.{h,cpp}` — L4 ncurses surface
  - `tui/tui.cpp`, `tui/tui_input.cpp` — composition + action registration
  - `completions.json` — `/skills` namespace
  - `src/main.cpp` — `amber-cli skills …` parity surface
- **Test files** (target): `tests/skills_browse_core_test.cpp`,
  `tests/skill_index_test.cpp`, `tests/skill_install_test.cpp`,
  `tests/tui_pty_test.cpp` (explorer flow, fake index)
- **Spec status**: design — no implementation yet.

---

### 0. Registry decision (blocking)

There are two sites with similar names and entirely different products.

| Site | What it is | Fit for amber |
|---|---|---|
| `skills.com` | Invitation-only community for people: career stories, forums, sponsors. No skill packages, no public API, no `SKILL.md`. | **None.** Not an agent-skill registry. |
| `skills.sh` | The Open Agent Skills ecosystem: a public index of skills, packs, topics and security audits, plus a public search API. Named in `skills/agent-skills.md` as the marketplace gap. | **Target.** |

Measured on 2026-10-25:

- `GET https://skills.sh/api/v1/skills` → `401 authentication_required`. The v1
  API (leaderboard, search, curated, detail, audit) requires a **Vercel OIDC
  token**, which only a Vercel-deployed app can mint. A native client cannot
  obtain one.
- `GET https://skills.sh/api/search?q=tdd&limit=20` → `200`, no auth. This is the
  endpoint the official `skills` CLI uses. It returns `id`, `source`, `name`,
  `installs` — **no description, no category**.
- Descriptions live in each `SKILL.md` frontmatter, reachable over
  `raw.githubusercontent.com` with no auth. Repos may publish
  `.claude-plugin/marketplace.json` with `category` + `keywords` for a whole
  repo (one cheap fetch).
- Non-GitHub publishers expose `/.well-known/agent-skills/index.json`
  (`name`, `description`, `digest`), with `/.well-known/skills/index.json` as
  the legacy fallback.

**Decision.** Ship a `SkillIndex` port with adapters, not a single hardcoded
client:

1. `SkillsShIndex` — unauthenticated `/api/search` for discovery (works today).
2. `GithubEnrichment` — repo tree + `SKILL.md` + `marketplace.json` for
   descriptions, categories, file lists and installs (works today).
3. `WellKnownIndex` — per-host `index.json` for non-GitHub sources.
4. `SkillsShV1Index` — optional, only when the user supplies
   `SKILLS_SH_TOKEN`; adds bulk views, audits and stable detail/hash.
5. `LocalIndex` — installed skills and the cache; always available, never
   fails.

`skills_index = none` disables all network adapters (air-gapped mode). The
explorer still lists installed skills.

---

### 1. Design principles

1. **Local first, network second.** Every keystroke filters what is already in
   memory. A server query is an enhancement that may fail, lag or be cancelled;
   it never blocks typing, selection or install.
2. **One screen to decide.** Search, results, evidence and install all live in
   the explorer. No modal stack for the common path.
3. **The row is the pitch.** Name, source, installs, trust and state on one
   line. The detail pane answers "why would I want this?"; the install sheet
   answers "what will it do to my machine?"
4. **Trust is evidence, not vibes.** Show source, audit verdicts, body preview,
   token cost, and a static risk scan. Say "not audited" rather than implying
   safety.
5. **No fabricated metrics.** If the registry gives no trend series, show no
   sparkline. If a facet cannot be applied server-side, say it applies to
   loaded results.
6. **Keyboard complete, mouse optional.** Every action has a key and a command
   equivalent (`ux-policy.md`). Wheel scrolls; nothing requires a click.
7. **Never surprise the session.** Installs are explicit, atomic and
   reversible; nothing auto-installs, auto-updates or auto-activates beyond
   what the user confirmed.

---

### 2. Command surface

#### 2.1 Placement

`skills` is a **core domain root** (allowed by
`tui/command-namespace.md` §4.4 step 5: "core, every-day domain → host root
allowlist"). `files` and `settings` are already plural roots, so the plural is
consistent. The existing `/set skills` (configuration) and `/get skills`
(readout) keep their meanings; they are not acquisition surfaces.

```
skills                            — open the explorer (last view)
skills browse                     — open the explorer
skills search <query>             — explorer, query prefilled
skills list                       — installed packages, table in scrollback
skills show <name>                — one skill's detail, readout
skills install <id|owner/repo|url> [--global] [--yes]
skills update [<name> | --all]    — check/apply updates
skills remove <name> [--global]   — remove an installed package
skills sources                    — index sources, freshness, cache size
```

- `enable` / `disable` / `block` stay under `/set skills` — they are curation
  state, not acquisition. The explorer calls the same backends (`x`, `b`).
- `skills refresh` is deliberately absent: catalog rescan is
  `/set skills refresh`, index revalidation is `R` in the explorer and
  `skills sources --refresh`.
- Bare `skills` is allowed because its action is registered
  (`core.skills`), per `tui/command-tree.md`.

#### 2.2 JSON nodes (shape only)

```json
"skills": {
  "action": "core.skills",
  "help": "Find, install and update skills.",
  "man": "Opens the Skills Explorer: search and browse the skills.sh index, inspect a skill's body, audit status and token cost, then install it into the project (.amber/skills) or globally (~/.config/amber/skills). Every screen action has a command form.",
  "short": "sk",
  "children": {
    "browse":  { "action": "core.skills.browse",  "help": "Open the explorer." },
    "search":  { "action": "core.skills.search",  "help": "Open the explorer with a query." },
    "list":    { "action": "core.skills.list",    "help": "List installed skill packages." },
    "show":    { "action": "core.skills.show",    "help": "Show one skill in detail." },
    "install": { "action": "core.skills.install", "help": "Install a skill." },
    "update":  { "action": "core.skills.update",  "help": "Check for or apply updates." },
    "remove":  { "action": "core.skills.remove",  "help": "Remove an installed skill." },
    "sources": { "action": "core.skills.sources", "help": "Show index sources and freshness." }
  }
}
```

No feed is required: result rows are not command-tree leaves (there are
millions), and the drawer is composed on the UI thread, where a network call is
forbidden. The single exception is `skills sources`, whose children are the
configured sources from cache and may be a feed.

#### 2.3 CLI parity

`ux-policy.md` requires a command equivalent for every UI action. The headless
surface prints text by default, JSON with `--json`:

```
amber-cli skills search <query> [--limit N] [--json]
amber-cli skills list [--json]
amber-cli skills show <id> [--json]
amber-cli skills install <id|owner/repo|url> [--global] [--yes] [--json]
amber-cli skills update [<name>|--all] [--check] [--json]
amber-cli skills remove <name> [--global]
amber-cli skills sources [--refresh] [--json]
```

`--yes` is required for non-interactive install of anything that is not
`official` (see §8); without it, a non-TTY run fails with a typed reason.

---

### 3. Information architecture

#### 3.1 Tabs (browse mode, empty query)

| Tab | Source | What it answers |
|---|---|---|
| **For you** | deterministic project signals + install history | "what fits this repo?" |
| **Trending** | `/api/search` + cached leaderboard; `view=trending` via v1 when available | "what is moving now?" |
| **Top** | leaderboard by installs | "what is proven?" |
| **Official** | curated owners (bundled list; v1 curated when authenticated) | "who makes the thing?" |
| **Collections** | bundled editorial sets (`data/skill-collections.json`) | "show me a themed set" |
| **Installed** | `SkillCatalog` + ledger | "what do I have?" |
| **Updates** | ledger hash ≠ remote hash | "what changed?" |

- **For you** is deterministic, never model-generated: detect
  `package.json` deps, `go.mod`, `Cargo.toml`, `pyproject.toml`, `Dockerfile`,
  test directories and the repo's primary language, map them to a small
  bundled table of search terms, and label the reason above the results
  (`Based on: React, TypeScript, PostgreSQL`). Empty project → hide the tab.
- **Collections** is honest editorial: a bundled, versioned JSON of named sets
  (`Agent workflows`, `Testing`, `Design systems`) each with a description and
  queries/ids. It works offline and is never described as a registry category.
- **Official** and **Trending** degrade to cached data with a `cached` badge;
  they never show an empty screen when cache exists.

#### 3.2 Search

- Input is a single line; `local-first` substring/fuzzy match over loaded rows
  runs on every keypress (no debounce, <16 ms for 500 rows).
- A server query fires after **250 ms idle** and **≥2 characters**, is
  cancellable, and only the newest request id is accepted.
- Multi-word queries are semantic on the registry; reflect that in the header
  (`semantic · 38 results`) rather than silently changing behaviour.
- Server results **replace** the browse set; local exact/prefix matches are
  pinned to the top and marked `local`. A one-line status says
  `12 local · 38 live`.
- Backspacing below 2 characters returns to the active tab, not an empty list.
- Recent searches (last 10, persisted) appear when the field is empty and
  focused; `Up` recalls.
- `Esc` in the field clears the query; `Esc` again closes.

#### 3.3 Facets

Left column, toggleable, applied client-side to the loaded result set:

| Facet | Values |
|---|---|
| Status | installed · update · not installed · unmanaged |
| Trust | official · audited · community · unverified · blocked |
| Category | from `marketplace.json` / `metadata.category` / bundled taxonomy |
| Source | top owners present in the current set |
| License | known values, `unknown` last |

When a live query is active, the header says `filters apply to loaded results`
— because `/api/search` accepts only `q`, `owner`, `limit`. Hiding that would
make filtering look broken at page boundaries.

Sort: `relevance` (default) → `installs` → `trending` → `name`. `s` cycles;
sorting is instant and local.

#### 3.4 Rows

```
▸ tdd                          mattpocock/skills     1.1M  ● ⌁ ✓ ◆
  Test-driven development. Use when the user wants to build features…
```

- Line 1: name (bold), source (dim), installs (right-aligned), badges.
- Line 2 (wide/medium only): sanitized description, one line, ellipsized.
- Narrow: two lines, no source column.
- Badges are glyph **plus** text in the legend, never colour alone:
  `●` installed · `⌁` update · `◆` official · `✓` audited · `!` risk ·
  `◇` unverified · `⧉` fork/duplicate.
- Duplicates are hidden by default (`isDuplicate` when authenticated; for
  GitHub, same-name same-owner forks are not detectable — do not guess).

#### 3.5 Detail pane

```
tdd                                          mattpocock/skills
1,144,149 installs · ▲12% · MIT · updated 3d ago
──────────────────────────────────────────────────────────────
Test-driven development. Use when the user wants to build
features or fix bugs test-first…
Installed · 9 files · 42 KB · update available (7f3a)
Audit  ✓ Socket   ✓ Snyk   ✓ Agent Trust Hub (LOW)
Cost   1.8k token body (36% of budget) · +1 discovery line
Pack   mattpocock/skills (38 skills)
──────────────────────────────────────────────────────────────
SKILL.md preview
# Test-Driven Development
TDD is the red → green loop…
```

Field order is deliberate: identity → popularity → fitness → **trust** → cost →
body. Missing fields collapse (no empty rows). `▲12%` appears only when the
registry supplies a change value; otherwise it is omitted.

The preview is the real markdown renderer (`tui/markdown.h`), scrollable, with
in-preview search (`/`, `n`/`N`). It is **not** a model summary; the user reads
the instructions the model would read.

---

### 4. Layout

Three breakpoints, all keyboard-equivalent:

| Terminal | Panes |
|---|---|
| ≥ 100 cols, ≥ 24 rows | facets · list · detail |
| 60–99 cols, ≥ 18 rows | list · detail; facets as an overlay (`f`) |
| < 60 cols or < 18 rows | list only; `Enter` opens full-screen detail; a size line explains the limit |

Wide (100×30):

```
┌ Skills ─────────────────────────────────────────────────────── [live] ┐
│For you │ Trending │ Top │ Official │ Collections │ Installed │ Updates│
│ / tdd                                                skills.sh · live │
├────────────────────┬──────────────────────────┬───────────────────────┤
│ STATUS             │ ▸ tdd              1.1M  │ tdd                   │
│  [x] All           │   mattpocock/skills ● ⌁ ✓│ mattpocock/skills     │
│  [ ] Installed 3   │   Test-driven develop…   │ 1.1M · ▲12% · MIT     │
│  [ ] Updates 1     │                          │ ───────────────────── │
│  [ ] Not installed │   grill-me         1.4M  │ Installed · 9 files   │
│                    │   mattpocock/skills ◆ ✓  │ update available      │
│ TRUST              │   Interview yourself…    │ Audit ✓ Socket ✓ Snyk │
│  [x] Official      │                          │ Cost 1.8k · +1 line   │
│  [x] Audited       │   code-review      795K  │ ───────────────────── │
│  [ ] Community     │   mattpocock/skills      │ # Test-Driven Dev…    │
│  [ ] Unverified    │   Review a branch…       │ Write the test first… │
│                    │                          │                       │
│ CATEGORY           │   …                      │                       │
│  [x] engineering   │                          │                       │
├────────────────────┴──────────────────────────┴───────────────────────┤
│ Enter detail · i install · p preview · d diff · / search · ? help     │
└───────────────────────────────────────────────────────────────────────┘
```

Medium (80×24): facets collapse; the footer gains `f filters`, and the header
drops the total count. Detail keeps the body preview but at 30 columns it wraps
hard — acceptable, because `p` opens the full pager.

States the layout must define explicitly:

- **Loading (first open):** header + tabs paint immediately; rows are dim
  placeholders (`·`), never a blocking spinner. `Esc` still closes.
- **Empty query, no cache:** `No index available` + `r retry` + `Tab to
  Installed`; the Installed tab still works.
- **No results:** `No skills match "…"` + three suggestions (`fewer words`,
  `browse Top`, `clear filters`).
- **Offline / stale:** one amber line under the header,
  `offline · cached 2h ago · search unavailable`, and `Esc` unaffected.
- **Rate limited:** `search paused 30s (registry limit)` with a live countdown;
  local filtering continues.
- **Error:** one line with the typed reason and `r retry`; the pane keeps its
  last good content.

---

### 5. Interaction

#### 5.1 Focus model

Three focusable regions: **search**, **list**, **detail**. `Tab`/`Shift+Tab`
cycles; `h`/`l` moves left/right; `/` always goes to search; `Esc` unwinds
detail → list → close. The focused region's border title brightens
(`P_BANNER`); the cursor is visible only in the search field.

#### 5.2 Keys

| Context | Key | Action |
|---|---|---|
| Global | `/` | focus search |
| | `1`–`7` | jump to tab |
| | `Tab` / `Shift+Tab` | cycle region |
| | `f` | facets (wide: focus column; narrow: overlay) |
| | `s` | cycle sort |
| | `R` | revalidate index cache |
| | `?` | help overlay (same key vocabulary as `Panel::show_help`) |
| | `q`, `Esc` | back / close |
| List | `j`/`k`, `↓`/`↑` | move |
| | `Ctrl+d`/`Ctrl+u`, `PgUp`/`PgDn` | page |
| | `g`/`G` | top / bottom |
| | `Enter` | detail (narrow: full-screen detail) |
| | `i` | install sheet |
| | `Space` | mark / unmark (multi-select) |
| | `a` | mark all visible |
| | `u` | update selected (Installed/Updates) |
| | `x` | enable/disable (calls `/set skills` backend) |
| | `b` | block (confirm) |
| | `D` | remove (confirm) |
| | `d` | diff installed vs remote |
| | `p` | full-body pager |
| | `v` | audit verdicts (when available) |
| | `A` | adopt an unmanaged installed skill (Installed tab) |
| | `o` | source URL (OSC 8 if supported, else printed) |
| | `y` | copy install command / id (OSC 52; prints if unsupported) |
| | `e` | edit installed `SKILL.md` (`$EDITOR` handoff) |
| Detail | `j`/`k`, wheel | scroll |
| | `/`, `n`/`N` | search within |
| | `[` / `]` | previous / next file |
| Install sheet | `Tab`/arrows | fields |
| | `Space`/`Enter` | toggle |
| | `p` | preview body (acknowledges review) |
| | `Enter` | install |
| | `Esc` | cancel (confirm if a write started) |

Wheel: scrolls the pane under the pointer, matching the existing
`BUTTON4/5` support. No click semantics — amber's mouse support is wheel-only
and adding click targets to a terminal UI is a separate decision.

#### 5.3 Install flow

`i` opens the install sheet (from the list or from detail). `Enter` on a list
row only opens detail; viewing never installs. A destructive side effect needs
its own explicit confirmation.

```
┌ Install skill ───────────────────────────────────────────────┐
│  tdd  ·  mattpocock/skills  ·  1.1M installs                 │
│                                                              │
│  Scope      (•) project  .amber/skills                       │
│             ( ) global   ~/.config/amber/skills              │
│  Name       tdd            ⚠ project tdd exists — replace?   │
│  After      (•) enable now  ( ) install disabled             │
│                                                              │
│  Evidence   ✓ audited (Socket, Snyk, ATH · LOW)              │
│             ⚠ 4 shell commands · 1 network URL · 0 hidden    │
│             body 1.8k tokens · +1 discovery line (~35/turn)  │
│             ⚠ review required — press p to read the body     │
│                                                              │
│  [Enter] install    [p] preview    [Esc] cancel              │
└──────────────────────────────────────────────────────────────┘
```

Rules:

- **Review gate.** For `community` / `unverified` sources with
  `skills_review_required = on` (default), `Enter` is inert until `p` has shown
  the body or the user toggles `install without reading`. A clear status line
  explains why, rather than a disabled-looking button.
- **Audit gate.** With `skills_audit_required = on`, an install whose audit is
  missing or `fail` requires an explicit override toggle; `fail` defaults to
  no.
- **Scope.** Default `project` (`<project>/.amber/skills`), because that is the
  shareable, reviewable location per `AGENTS.md`. `global` is one keystroke
  away and remembered for the session.
- **Name collision.** Same directory name already present in a scanned root:
  show which one (`project tdd` shadows `global tdd`), then offer
  `replace` (default, with diff), `install to other scope`, or `cancel`.
  Silent rename is not offered — the catalog is name-keyed and the model routes
  by name.
- **Enable now.** Installed-but-disabled is a first-class outcome: it costs no
  discovery budget and can be enabled later from `/set skills`.
- **What is written** is stated in plain words (`~/.config/amber/skills/tdd/`,
  9 files, 42 KB). No hidden directories.

Progress replaces the sheet in place:

```
Installing tdd → ~/.config/amber/skills/tdd
  [██████████░░░░░░]  6/9 files · 28 KB
  SKILL.md ✓  tests.md ✓  mocking.md ✓  …
  Esc cancel
```

`Esc` cancels the in-flight fetch, removes the temp directory, and reports
`install cancelled — nothing written`. A write cannot be partially applied:
files land in `.install-<slug>-<rand>/` inside the destination root and are
renamed into place only after every file is fetched, every path is validated,
and the digest (when supplied) matches.

On success: `installed tdd · 9 files · 42 KB · enabled`, plus the new
discovery line and the updated budget (`14 lines · ~480 tok/turn of 20`). The
catalog refreshes automatically; `read_skill` can activate it on the next turn.

#### 5.4 Multi-select

`Space` marks rows; the footer shows `3 marked · i install all`. The sheet then
installs in sequence with per-skill results (`3 installed, 1 skipped: name
collision`). Failures never abort the batch. Bulk install is the only place a
non-zero exit is aggregated, and it is reported, never silent.

#### 5.5 Update and diff

The **Updates** tab lists ledger entries whose recorded hash differs from the
remote. `d` opens a pager:

```
tdd  ·  installed 2026-10-02 (7f3a)  →  remote 2026-10-24 (b91c)
──────────────────────────────────────────────────────────────
  SKILL.md
- Write the test first, then the implementation.
+ Write the test first, watch it fail, then the implementation.
+ ## Seams
+ Tests live at seams, never against internals.
  +2 files (tests.md, mocking.md)
```

`u` applies after the diff (or opens it first when `skills_update_review = on`,
the default for non-official sources). A skill with local edits is flagged
`locally modified` and updates require `--force` / an explicit toggle; amber
does not overwrite work it did not write. `U` applies every reviewed update and
reports each outcome. Checks are explicit: `manual` (the default) never spends a
request on open, `never` refuses them, and `daily` throttles to one check a day
once the check cache lands (#407) — until then it follows the explicit
triggers.

#### 5.6 Remove

`D` → confirmation naming the directory, file count and size. If the skill is
activated in the current session, the dialog adds `active in this session until
the conversation ends` (activation is per-agent state, not re-read per turn).
Removal deletes the directory and the ledger entry; it never touches the
learned store or overrides (a `block` override survives, by design).

---

### 6. Visual language

- Reuse `rich::Line` and the existing pairs: name `P_ASSISTANT`, source
  `P_STATUS` dim, installs `P_BAR_DIM`, selected row reverse, installed
  `P_ACTIVE`, update `P_GAUGE_WARN`, risk `P_GAUGE_CRIT`, official `P_MD_LINK`,
  unverified `P_REASONING`. No new colour pair unless a legend requires it.
- Glyphs go through the existing glyph fallback (`display/icon-rendering.md`):
  ASCII mode renders `[inst] [upd] [off] [ok] [!]` instead of `● ⌁ ◆ ✓`.
- Numbers are compact and lowercase-consistent with the rest of the UI:
  `1.1M`, `795K`, `4.2k`; percentages only from real data (`▲12%`).
- Truncate rows with `…` by `text::display_cols`, never mid-glyph; detail wraps.
- Remote strings are rendered as **text only**: the sanitizer in §8 runs
  before anything reaches `rich::Run`.
- A one-line legend is available in `?`, not permanently on screen.

---

### 7. Data sources and constraints

| Source | Endpoint | Auth | Returns | Limits | Cache |
|---|---|---|---|---|---|
| skills.sh search | `GET /api/search?q=&limit=` | none | id, source, name, installs, count | undocumented; treat as tight | 60 s |
| skills.sh v1 | `/api/v1/skills`, `/search`, `/curated`, `/{id}`, `/audit/{id}` | Vercel OIDC bearer | views, file tree, hash, audits | 600/min | per `Cache-Control` |
| GitHub tree | `/repos/{owner}/{repo}/git/trees/{ref}?recursive=1` | none (optional token) | paths + blob SHAs | 60/h/IP unauthenticated | 1 h |
| GitHub raw | `raw.githubusercontent.com/{owner}/{repo}/{ref}/{path}` | none | file bytes | CDN | by blob SHA |
| marketplace.json | `raw…/.claude-plugin/marketplace.json` | none | repo category, keywords, owner | one per repo | 6 h |
| well-known | `https://host/.well-known/agent-skills/index.json` | none | name, description, digest, url | per host | 1 h |

Consequences that shape the UX:

- **Descriptions are lazy, not bulk.** No endpoint returns all descriptions.
  The list renders from cheap fields immediately; the selected row's detail is
  enriched first, then visible rows at concurrency 1–2. A description that has
  not arrived is `…` dim, never invented.
- **Categories are ours.** Derived, in order: `metadata.category` in the
  `SKILL.md` frontmatter → repo `marketplace.json` `category`/`keywords` →
  bundled keyword taxonomy. Uncategorised is a visible bucket, not a hole.
- **GitHub's 60/h unauthenticated budget is the real ceiling.** One tree call
  per repo, cached; raw fetches are not rate-limited the same way and are
  cached by blob SHA. `GITHUB_TOKEN`/`GH_TOKEN` is read from the environment
  when present (never required, never written to disk by amber).
- **The v1 API is optional.** When `SKILLS_SH_TOKEN` is set, the adapter gains
  bulk leaderboard/curated/audit and stable hashes; the UI marks the source
  `live+` instead of `live`. Without it, audits read `not audited` and updates
  compare GitHub blob SHAs.
- **No telemetry.** Amber sends only the query string to the registry. That is
  disclosed by `skills sources`, and `skills_index = none` stops it entirely.

---

### 8. Security and trust

A `SKILL.md` is untrusted instruction text that the model will follow. The
existing posture (`workspace/security-model.md`, `mcp/mcp-security.md`) applies:
amber does not execute skills, and `bash` stays approval-gated. The residual
risk is **prompt injection and social engineering**, so acquisition must make
the text visible before it is trusted.

1. **Sanitize every remote string.** Strip C0/C1 controls, ESC/CSI/OSC/DCS,
   bidi overrides (`U+202A–202E`, `U+2066–2069`), zero-width characters, and
   overlong lines before rendering or persisting in the ledger. Without this a
   skill name can rewrite the terminal or forge UI. Sanitizing at the edge also
   keeps the pure renderer free of escape handling.
2. **Static risk scan** on the body and every file: shell fences/commands,
   URLs, `curl | sh`, `sudo`, base64 blobs, references outside the workspace,
   and classic injection phrases (`ignore previous instructions`). Report
   counts and line numbers in the sheet and the `p` pager. It is advisory —
   say "no signals found", never "safe".
3. **Trust tiers.** `official` (curated owner) → `audited` (all partners pass)
   → `community` (known source, no audit) → `unverified` (unknown source or
   no metadata). `blocked` never installs. The tier is on every row; the tier
   rule is one function, not scattered booleans.
4. **Review gate** (§5.3) is on by default for `community`/`unverified`.
5. **Audit surfacing.** When available, show partner, verdict and risk level
   verbatim; link the detail page. `404 no audits` reads `not audited`, not
   `passed`.
6. **Path safety.** Reject absolute paths, `..`, symlinks, device files; cap
   file count (200) and total bytes (10 MB); write only under the destination
   root; no executable bit propagation; never follow a symlink out of the temp
   dir.
7. **Integrity.** Verify the well-known `digest` (sha256) when present; for
   GitHub, compare the fetched blob SHA against the tree's. A mismatch aborts
   the install and removes the temp dir.
8. **Provenance ledger.** `<skills root>/.lock.json` records id, source, slug,
   ref, hash, file count, bytes, audit snapshot, installed_at and enabled
   state. It travels with a shared project skill directory, which is what makes
   "where did this come from?" answerable later.
9. **No silent activation.** Installing with `enable now` is the only path that
   adds a discovery line; the sheet says so, with the token estimate.

---

### 9. Architecture mapping

| Layer | Unit | Responsibility |
|---|---|---|
| lib | `SkillIndex` (abstract) | `search`, `browse`, `detail`, `audit`, `sources`; all return value types, all take a `CancellationToken` |
| lib | `SkillsShIndex` | unauthenticated `/api/search`; optional bearer v1 |
| lib | `GithubEnrichment` | tree, raw files, `marketplace.json`, categories |
| lib | `WellKnownIndex` | `index.json` parsing + digest |
| lib | `SkillLedger` | read/write `.lock.json`, atomic replace, hash compare |
| lib | `install_skill_tree` | validate → temp → fetch → digest → rename → ledger |
| L1 | `SkillsBrowseCore` | rows, tabs, facets, filter, sort, focus, selection, scroll, key → intent; **no ncurses, no I/O** |
| L2 | `SkillsService` | request ids, 250 ms debounce (ClockPort), cancellation, worker handoff, view-model snapshot, cache reads |
| L4 | `SkillsExplorer` | `Dialog` paint loop, `rich::Line` assembly, markdown preview pager, install sheet |
| L5 | `Tui` | construct, wire, open/close, action registration |

Rules that follow from `tui/architecture.md`:

- The render path is a pure function of the `SkillsBrowseCore` snapshot; it
  never issues a request. Key handling returns **intents** (`Search(q)`,
  `OpenDetail(id)`, `Install(...)`) that L2 executes.
- One worker owns index I/O. Responses carry the request id; a stale id is
  dropped. The UI thread never blocks on the network, a lock or a file read
  larger than the cache.
- File fetches during install run at concurrency 4 with a shared
  `CancellationToken`; the UI keeps painting progress from an atomic counter.
- Closing the explorer cancels in-flight reads; an in-flight install asks
  before cancelling because it is mid-write.
- The L1 unit tests must compile without ncurses; the L1 test binary does not
  link `NCURSES_CFLAGS`.

Configuration (all under `/set skills`, never in the explorer):

```
skills_index = skills.sh          # skills.sh | none | <base-url>
skills_index_token_env = SKILLS_SH_TOKEN
github_token_env = GITHUB_TOKEN
skills_install_scope = project    # project | global
skills_review_required = on
skills_audit_required = off
skills_update_review = on
skills_update_check = manual      # manual | daily | never
```

Storage:

- Cache: `$XDG_CACHE_HOME/amber/skill-index/` — `catalog/`, `repos/`,
  `details/`, `search/` (TTL per §7), each with ETag/Last-Modified.
- Ledger: `<project>/.amber/skills/.lock.json` and
  `~/.config/amber/skills/.lock.json`.
- Last view/tab/sort/facets: project settings; recent searches: global cache.

---

### 10. Scenarios

#### [SC-01] First open with no network

- **Given**: no cache, index unreachable, 3 skills installed
- **Input**: `/skills`
- **Expected**: explorer paints; Installed tab shows 3; header says
  `offline`; search reports the typed failure with `r retry`; no blocking
  spinner; `Esc` closes.
- **On failure**: empty screen, frozen UI, or a network call on the UI thread.

#### [SC-02] Search is local-first and cancellable

- **Given**: 200 cached rows; slow registry
- **Input**: type `tdd` one character at a time, then `TeSt`
- **Expected**: each keystroke filters instantly; one request fires after the
  debounce; earlier in-flight requests are cancelled; only the newest result
  set is shown; `12 local · 38 live` is reported.
- **On failure**: laggy typing, out-of-order results, or a request per
  keystroke.

#### [SC-03] Description fills in without reflow

- **Given**: a GitHub result row with no description
- **Input**: move the cursor onto it
- **Expected**: cheap fields paint immediately; frontmatter arrives and fills
  the description in place; the row height does not change; a failed enrich
  leaves `no description` and a retry on `R`.
- **On failure**: placeholder replaced by an invented summary, or a layout jump.

#### [SC-04] Install is atomic

- **Given**: a multi-file skill, network drops after 6/9 files
- **Input**: `i` → `Enter`
- **Expected**: temp dir removed; destination untouched; ledger unchanged;
  `install failed: fetch 7/9 (network)`; retry is safe.
- **On failure**: partial skill on disk, or a ledger entry for nothing.

#### [SC-05] Name collision is disclosed

- **Given**: global `tdd` installed; installing `tdd` at project scope
- **Input**: `i`
- **Expected**: sheet says `project tdd will shadow global tdd`; options
  replace / other scope / cancel; empty directories are never created for a
  cancelled install.
- **On failure**: silent shadowing or a rename the model cannot route.

#### [SC-06] Review gate holds

- **Given**: `skills_review_required = on`, unverified source
- **Input**: `Enter` on Install without preview
- **Expected**: nothing installed; status explains `press p to read the body
  first`; `p` opens the body, `Enter` then installs.
- **On failure**: install proceeds with the body never shown.

#### [SC-07] Audit failure blocks by default

- **Given**: `skills_audit_required = on`, audit `fail`
- **Input**: `i`
- **Expected**: install disabled with `audit: fail (Socket)`; an explicit
  override toggle is required and is recorded in the ledger note.
- **On failure**: a failing skill installs with a warning only.

#### [SC-08] Hostile metadata cannot forge the UI

- **Given**: a skill name containing `\x1b[2J`, RTL overrides and a 4 KB line
- **Input**: search results render
- **Expected**: controls are stripped, the name is truncated by display width,
  the terminal is not cleared, and the ledger stores the sanitized name.
- **On failure**: escape sequence reaches the terminal.

#### [SC-09] Update shows a diff before applying

- **Given**: installed `tdd`, remote hash differs, local file unmodified
- **Input**: `d` then `u`
- **Expected**: unified diff of `SKILL.md` plus added/removed files; apply
  rewrites atomically and updates the ledger hash.
- **On failure**: blind overwrite or a stale hash after update.

#### [SC-10] Local edits are protected

- **Given**: installed skill edited by the user, remote updated
- **Input**: `u`
- **Expected**: `locally modified` badge; update requires `--force` or an
  explicit toggle; the diff marks the user's lines.
- **On failure**: user work overwritten silently.

#### [SC-11] Rate limit does not freeze the UI

- **Given**: 429 with `Retry-After: 30`
- **Input**: continue typing and navigating
- **Expected**: local filtering and Installed tab keep working; header shows
  `search paused 30s` with a countdown; one retry after the window.
- **On failure**: modal error, retry storm, or dead keys.

#### [SC-12] Stale cache is labelled

- **Given**: cache age 5 h, index unreachable
- **Input**: open Trending
- **Expected**: rows paint with `cached 5h` in the header; no `live` claim.
- **On failure**: stale data presented as current.

#### [SC-13] Multi-select batches safely

- **Given**: 4 marked, one collides, one fails its digest
- **Input**: `i` → confirm
- **Expected**: 2 installed, 1 skipped (`name collision`), 1 failed
  (`digest mismatch`); one summary line; temp dirs cleaned.
- **On failure**: batch aborts on first error or reports a false success.

#### [SC-14] Discovery budget is visible

- **Given**: 19 of 20 discovery lines used
- **Input**: install a skill with `enable now`
- **Expected**: sheet warns `+1 line → at cap`; install still succeeds (the cap
  is enforced by the catalog, not the installer), and the success line names
  the budget.
- **On failure**: the budget is discovered later as a missing skill.

#### [SC-15] Narrow terminal degrades, not breaks

- **Given**: 58×17
- **Input**: `/skills`
- **Expected**: single-pane list; `Enter` opens detail; a size hint suggests
  enlarging; no clipped borders or negative-width draws.
- **On failure**: crash or unreadable overlap.

#### [SC-16] Headless parity

- **Given**: no TTY
- **Input**: `amber-cli skills search tdd --json`
- **Expected**: stdout is a JSON array with the same rows the TUI shows;
  exit 0; no ANSI; no network call when `skills_index = none`.
- **On failure**: interactive prompt, or a different result model.

#### [SC-17] Path traversal is rejected

- **Given**: an archive/tree entry `../../.ssh/config`
- **Input**: install
- **Expected**: entire install rejected with `unsafe path`; nothing written.
- **On failure**: file escapes the destination root.

#### [SC-18] Interrupted install cleans up

- **Given**: install in progress
- **Input**: `Esc`, then `q`
- **Expected**: cancel confirmation; temp dir removed; no ledger entry; the
  explorer closes only after cleanup.
- **On failure**: orphaned temp directory.

#### [SC-19] Unmanaged skill can be adopted

- **Given**: `~/.config/amber/skills/handwritten/` with no ledger entry
- **Input**: Installed tab
- **Expected**: row shows `unmanaged`; `A` computes the hash and adds a ledger
  entry so future updates are detectable; nothing is rewritten.
- **On failure**: unmanaged skills silently look updatable, or the adopt action
  rewrites files.

#### [SC-20] Block override prevents install

- **Given**: `/set skills block some-skill`
- **Input**: `i` on that skill
- **Expected**: install refused with the override note; no scope choice.
- **On failure**: block applies only after installation.

#### [SC-21] Sorting and facets are honest under a live query

- **Given**: live search, facet `Official` selected
- **Input**: scroll to the end
- **Expected**: header says `filters apply to loaded results`; no claim that
  the registry was filtered.
- **On failure**: a filter that silently under-reports the registry.

#### [SC-22] Every UI action has a command

- **Given**: any explorer action
- **Input**: `?`
- **Expected**: help lists the equivalent `/skills …` or `/set skills …`
  command; the command produces the same effect headlessly.
- **On failure**: a UI-only capability.

---

### 11. Phases

| Phase | Scope | Explicitly not included |
|---|---|---|
| **P0** | `/skills` root; explorer with Top/Installed/Updates; search (`/api/search`); lazy GitHub enrichment; install sheet with review gate; atomic tree install; ledger; remove; CLI parity; sanitizer | facets, tabs beyond three, audits, diff, multi-select |
| **P1** | For you / Trending / Official / Collections; facets + sort; categories from `marketplace.json`; updates with diff and local-edit protection; audit badges; cache/offline/429 states; multi-select; adopt | v1 auth, packs, publishing |
| **P2** | Well-known sources; packs (`owner/repo` bulk); optional `SKILLS_SH_TOKEN` (bulk views, audits, stable hashes); in-session preview ("try for one turn"); publish/export flow | accounts, payments, code execution |

P0 is deliberately shippable in one PR series and useful without any token.

---

### 12. Test plan

- **L1 pure** (`tests/skills_browse_core_test.cpp`): tabs, filter, facet
  predicates, sort stability, selection/scroll invariants, key → intent for
  every key, narrow/wide layout maths, empty/loading/error states as data.
- **lib** (`tests/skill_index_test.cpp`): parse real recorded fixtures
  (`/api/search`, tree, `marketplace.json`, `index.json`); malformed JSON;
  401/404/429/503 typed errors; cache TTL and ETag behaviour; sanitizer
  corpus (ANSI, bidi, zero-width, overlong); no live network — inject a
  `HttpGet` function.
- **install** (`tests/skill_install_test.cpp`): flattening nested skill dirs,
  name validation, collision policy, path traversal, symlink, file/size caps,
  digest mismatch, interrupted fetch cleanup, ledger round-trip and atomic
  replace.
- **pty** (`tests/tui_pty_test.cpp`): open → search → detail → install sheet →
  review gate → install against a fake index; narrow-terminal degradation;
  `Esc` during a fetch.
- **CLI** (`tests/command_line_test.cpp` or new): `--json` shape, exit codes,
  `--yes` refusal for unverified, `skills_index = none`.

Hermetic by construction: unit tests never hit skills.sh or GitHub.

---

### 13. Non-goals

- Not a general package manager; only `SKILL.md` packages (and their support
  files) are installed.
- No background auto-update, no silent install, no install-on-open.
- No accounts, sign-in, payments or private registries in P0–P2.
- No embedded browser, image preview or web view.
- No model-generated summaries of a skill's body — the user reads the text
  that will be injected.
- No MCP/plugin marketplace in this surface; the same explorer can host them
  later behind separate ports.
- No inferred trend metrics when the registry supplies none.

---

### 14. Open questions

1. **Registry**: is `skills.sh` the intended target, or did `skills.com` mean
   something else? Everything above assumes skills.sh; the adapters make a
   second index possible, but the row/ranking design is tuned to installs and
   audits.
2. **Auth**: is an optional user-supplied `SKILLS_SH_TOKEN` acceptable
   (unlocks audits and bulk views), or must the feature ship token-free?
3. **Root namespace**: confirm `skills` as a core allowlisted domain (the
   grammar treats roots as scarce). Alternative: `/plugin skills`, which costs
   discoverability.
4. **Default scope**: project (proposed) vs global.
5. **Review gate default**: on (proposed) vs opt-in.
6. **Name collisions**: replace-with-diff (proposed) vs refuse-and-rename.
7. **`.agents/skills`**: should amber scan the emerging universal directory?
   That is a catalog change, not this surface, but it changes what "installed"
   means.

---

### Cross-references

- **Depends on**: `skills/agent-skills.md` (umbrella, trust posture),
  `skills/skill-files.md` (format/discovery), `skills/skill-catalog.md`
  (catalog, overrides, budgets), `tui/architecture.md` (layers, threading),
  `tui/command-tree.md` + `tui/command-namespace.md` (root grammar),
  `tui/dialogs.md`, `tui/menu-select.md`, `display/markdown-parser.md`,
  `display/icon-rendering.md`, `workspace/security-model.md`,
  `config/ui-config.md`
- **Depended on by**: `docs/skills-tracker.md` (new phase), `INDEX.md`,
  GitHub epic #391 and sub-issues #392–#412
- **Test coverage**: planned per §12

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-10-25 | Initial UX/architecture spec; measured registry constraints |

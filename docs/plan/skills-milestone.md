# Skills acquisition — completion milestone (M-skills)

One branch (`feat/skills-complete`), one PR when every issue below is finished.
Commits are the tasks; nothing merges until the acceptance lists in the issues
are met and the whole surface has been exercised by hand.

The five-feature plan that shipped earlier (`docs/skills-completion-plan.md`)
delivered the settings, the shell, update review, batch marks and the universal
root. It did **not** finish the epic: `#397`, `#398`, `#405`, `#406`, `#407`,
`#408` and `#409` each have acceptance criteria that were left open, and the
plan's "landed" rows overstated them. This milestone closes that gap.

## Order (each block is commits, not a PR)

| # | Issue | The work | Why here |
|---|---|---|---|
| 1 | #406, #405 | Bundled data (`data/skill-collections.json`, `data/project-signals.json`), `project_signals()`, `load_collections()`, facet model with AND/OR semantics in `tui/skills_browse_core` | Pure, testable, and the shelves cannot exist without it |
| 2 | #405 | For you / Trending / Top / Official / Collections as real shelves with honest labels; per-tab scroll and selection | Needs (1) |
| 3 | #398 | Detail pane blocks: evidence, cost, audit; markdown preview through `rich`; file list; `o`/`y` with OSC 8/52 fallbacks | Needs the detail data already fetched |
| 4 | #397 | Responsive states and persistence: three breakpoints, `Esc` unwinding, `live`/`cached Nh`/`offline`, last tab/sort/facets, help from the key table | Paints what (2) and (3) produce |
| 5 | #407 | Degradation: cache across restart, 429 without freezing input, `Retry-After` honoured, a next action in every state | Needs the header states from (4) |
| 6 | #408 | Batch: marks survive filtering, 2/1/1 reporting, mid-batch cancel, audit verdict display, verdicts in the ledger | Needs (3) for verdicts |
| 7 | #409 | Well-known index (`$schema` validated), digest mismatch leaves nothing on disk, repo bulk install with per-skill outcomes, path-scoped URL errors | Independent, largest remaining |
| 8 | #410 | **Not implemented.** Conditional on Vercel OIDC; it cannot be verified anonymously, so it stays a documented conditional rather than a claim | Scope decision |

## Acceptance = the issue text

Each block's commits must satisfy the acceptance list in its issue, not a
summary of it. Where an item cannot be tested hermetically (OSC 8/52, a live
429), the test asserts the fallback path and the PR says which item is manual.

## Manual test pass before the PR

At the end, with the fixed local endpoint up, one session drives: search →
detail → preview → install → update review → remove, plus each shelf, each
facet combination, a forced offline/429, and a batch with a collision. The PR
body records what was driven by hand and what only by test.

## Progress on `feat/skills-complete`

Landed and pushed (each commit green: `make`, unit suite, PTY suite,
complexity, class-size, duplicates, format, clang-tidy):

- **`data/`**: `project-signals.json` (repository fact → queries) and
  `skill-collections.json` (editorial shelves). Both loaders are tolerant: a
  missing or malformed file is an empty shelf with a reason, never a crash.
- **`agent::detect_project_signals`**: substring/`is_directory` detection, no
  model call, reasons and queries in table order, deduplicated.
- **Facet model** (`tui/skills_browse_core`): four groups (status, trust,
  category, source), OR within a group, AND across groups, a visible
  `uncategorised` bucket, and counts that apply every *other* group so a
  selection keeps a truthful count.
- **Facet pane**: `f` focuses it, `j`/`k` walk values and wrap between groups,
  Enter/Space toggles, `r` resets, Esc returns; narrow terminals get the same
  state machine in a modal. The column renders groups, counts and `[x]` marks.

- **#408 (model half)**: `AuditVerdict` on the detail, the row badge
  (`✓`/`!`/`✗`, nothing when unaudited), the sheet's verdict text, and the
  ledger recording the verdict shown at install time. `batch_report()` is pure
  and reports installed/skipped/failed separately, with a `cancelled` suffix;
  `Esc` during a batch cancels after the current skill instead of closing the
  explorer. Marks are keyed by id and survive filtering and refresh.
- **#405 (state half)**: every shelf keeps its own cursor and scroll for the
  session, so 1..7 and back returns to the row you were reading.

- **#405 (shelves)**: a shelf runner over `SkillsService` (one query at a time,
  pages merged by id via `merge_shelf_rows`), `For you` from the repository's
  own signals with a `Based on: …` reason, `Official` searching the curated
  owners **and** dropping results whose owner is not on the list, `Collections`
  listing editorial shelves whose Enter runs their queries, and `Trending`
  saying `no trend signal` rather than inventing one. Collection rows refuse
  install/update with a sentence pointing at the skills they list.

- **#398 (detail pane)**: evidence, cost and audit blocks composed once per
  detail (audit verdict + risk count; token estimate through the catalog's own
  `estimate_skill_tokens`, share of `skills_body_budget_tokens`), the file list
  with `[`/`]` walking previewable files (fetched on demand through a `Files`
  job; a binary is never rendered as text), and `p` opening the body in a pager
  that uses `md::render` with `/` search and `n`/`N`. `o` prints the source URL
  and `y` the install command; OSC 8/52 are emitted only where the terminal
  advertises them, and the plain text reaches the transcript either way.
- **#397 (shell states)**: the header claims `idle` until a request answers,
  `live` only after a successful one, `cached <age>` from the page's fetch
  time and `offline · cached <age>` when the cache is all that answered; a 429
  countdown ticks without blocking keys; `h`/`l` move focus; help is rendered
  from `skill_key_table()` (a test pins the action keys against it); the last
  tab/sort/facets restore from `<state>/skills-ui.json`; below 60 columns the
  footer says why the pane is list-only.
- **#407 (degradation)**: cross-restart cache and corrupt-cache behaviour are
  pinned in the index suite, `Retry-After` is carried on the failure and armed
  as one non-blocking automatic retry, a 5xx/timeout gets one backoff retry in
  the adapter (a 4xx never does), `skills_index=none` answers locally with the
  config hint, and the header never prints `live` without a successful request.
- **#408 (batch + audit)**: `BatchOutcome` is the accumulator, so 2/1/1 is a
  unit-tested property rather than a paint-loop accident; audit badges come
  from one `audit_badge()` so `!`/`✗`/`✓` and "nothing when unaudited" cannot
  drift; the ledger records the verdict shown at install time.
- **#405 (ids)**: a collection that carries ids is a shelf of rows that works
  offline; its query results and pins merge by id.
- **#409 (well-known + repo)**: `WellKnownIndex` parses the v0.2 `url`/`digest`
  and v0.1 `files` shapes, rejects an unknown `$schema` by name, resolves
  relative URLs under the host or the path-scoped base, verifies every file's
  sha256 before use (a mismatch leaves nothing to install), and answers
  `Unsupported` for search so a non-GitHub source appears only through an
  adapter that supplies it. `GitHubIndex::list_skills` plus
  `skills install owner/repo --yes` installs every skill in a repository and
  reports installed/skipped/failed without aborting on one bad skill.
- **#410** stays a documented conditional: Vercel OIDC cannot be exercised
  anonymously, so claiming it shipped would be a claim the tests cannot back.

Held back (deliberately):

- The PTY proof of a full 2/1/1 *batch* is the one acceptance item covered by
  unit tests (`BatchOutcome`, `batch_report`) and by the PTY's 1-skill skip
  path, not by a four-skill PTY run: the mirror would need three more skill
  endpoints for a behaviour the accumulator already owns. The PR says so.
- Detail-pane in-body search (`/` inside the pane, `n`/`N`) is in the pager
  (`p`), not in the pane itself; the issue lists it under the preview surface.

Every commit is green: `make`, the unit suite (1,604), the PTY suite,
complexity, nesting, class-size, duplicates, format-check-changed, clang-tidy
and cppcheck (`make analyze`).

The PTY suite now covers the three breakpoints the issue names (120x40 with
facets, 80x24 list+detail, 58x17 list-only with the size hint), a restored
shelf across a reopen, and an explicit command overriding it. The live
registry was driven by hand for the CLI surface (`skills sources`, `search`,
`show`); the TUI flows are driven by the PTY scenarios above.

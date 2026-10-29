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

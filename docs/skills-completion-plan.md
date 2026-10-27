# Skills acquisition — completion plan

Tracks the work that finishes epic #391 after v0.8.0. One **feature** is one
branch and one PR; each task below is a commit inside it (red test, then green
fix). The plan file is updated as features land.

| Feature | Closes | State |
|---|---|---|
| **F1 settings & truth** — the documented gates, scope and index become real; docs/help/website/MISSION stop overstating | #401, #411 residual | in progress |
| **F2 browse surface** — shelves, facets, responsive layout | #397, #405, #406 | queued |
| **F3 update review** — diff pager, update-all, update settings | #402 | queued |
| **F4 resilience & batch** — offline/stale/429, multi-select | #407, #408 | queued |
| **F5 sources** — `.agents/skills`, packs/well-known; v1 conditional | #409, #412 (#410 conditional) | queued |

## F1 tasks

- [x] Config: `skills_index`, `skills_install_scope`, `skills_review_required`,
      `skills_audit_required`, the two token env names; validation, save, tests.
- [x] Explorer/sheet honour the settings live; default scope from config.
- [x] CLI honours index and scope, with `--project` to override a global default.
- [x] `/set skills review|audit|scope|index` and `/get skills settings` in the
      command tree.
- [x] Truth pass: `--help`, website manual + acquisition page, `MISSION.md`,
      tracker, milestone, and close #401/#411.

## Evidence the plan rests on

- The spec (§7) documents six acquisition keys; `Config` had three unrelated
  `skills_*` keys and `completions.json` exposed none of the acquisition ones.
- `draw_tabs` renders 2 of the 7 `SkillTab` values; `skills_layout()` had no
  production caller; `apply_tab` answered “that shelf is not wired yet”.
- No line-diff utility existed for #402’s pager.
- `install_skill_pack()` already handled tar.gz/path/URL, so #409’s pack half
  was done.
- #410 needs Vercel OIDC and cannot be verified anonymously, so it stays
  conditional and does not gate the release.

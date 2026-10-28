# Skills acquisition — completion plan

Tracks the work that finishes epic #391 after v0.8.0. A milestone is one branch
and one PR; each feature below is a commit group inside it, and each task is a
commit (red test, then green fix). The plan file is updated as features land.

| Feature | Closes | State |
|---|---|---|
| **F1 settings & truth** — the documented gates, scope and index become real; docs/help/website/MISSION stop overstating | #401, #411 residual | landed (#470) |
| **F2 browse surface** — shelves, facets, responsive layout | #397 (shell), #405 (Updates shelf), #406 (facets/sort) | landed (#471) |
| **F3 update review** — diff pager, update-all, update settings | #402 | landed (#479) |
| **F4 resilience & batch** — offline/stale/429, multi-select | #407, #408 | landed (#479); caching still #407 |
| **F5 sources** — `.agents/skills`, packs/well-known; v1 conditional | #409, #412 (#410 conditional) | landed (#481); well-known still #409 |

## F5 tasks

- [x] `.agents/skills` as a universal, opt-in, read-only scan root with a
      precedence of project → global → universal → interop (#412).
- [x] `SkillScope::Universal` labels it in the catalog, the explorer and
      `/skills list`; removal stays refused for it, as for interop.
- [ ] `#409`'s well-known index and pack installs: the pack half already shipped
      (`install_skill_pack` handles tar.gz, path and URL); the well-known index
      is not built.
- [ ] `#410`: still conditional on Vercel OIDC and cannot be verified anonymously.

## F4 tasks

- [x] Marks: `Space` toggles, `a` marks the visible rows, the footer counts them,
      rows paint the mark, `i` installs the marked set.
- [x] `tui::plan_batch`: collisions and unread community sources are skipped by
      name; official sources bypass the review gate; a batch summary reports both
      halves.
- [x] Degradation text names the cause and the way out: `offline · R retries`,
      `rate limited (retry in Ns)`, `[cached]` for stale answers.
- [ ] Cached search/detail reuse and a per-source backoff (#407 proper) — the
      transport still re-asks on every explicit retry.

## F3 tasks

- [x] `agent::diff_lines` / `render_unified_diff`: bounded line diff with hunks.
- [x] `agent::build_update_review`: body diff, added/removed files, local-edit flag.
- [x] Explorer: `u` reviews then applies, `d` diffs alone, `U` applies every
      reviewed update, `?` help lists them.
- [x] Local edits need the second `u`; `skills_update_review` and
      `skills_update_check` are real settings.
- [x] CLI: `--diff`, `--force`, `--all`, `--check`, and a non-zero exit when an
      update is refused.
- [x] Docs: guide, spec, website, manual, command tree.

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

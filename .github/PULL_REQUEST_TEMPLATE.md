## Summary

Describe the **completed feature or bug fix** this PR delivers and why. Link the
architecture proposal (the issue, a `docs/plan/` entry, or an inline spec). One
PR delivers one finished feature or bug; the tasks from its issue tree are
commits on this branch, not separate PRs.

## PR readiness

- [ ] This is a completed feature or bug fix, not a task slice or a draft.
- [ ] Every task is green and `make verify` is clean.
- [ ] The branch has been pushed as work completed; this PR is the first request
      for review, not a progress board.

## Manual end-to-end verification

The PR is not ready until this can be filled in. A unit or integration test is
not manual E2E.

- **Steps:**
  1.
- **Environment:** (OS, terminal, provider/model, commit)
- **Expected:**
- **Observed:**
- **Evidence:** (command output, screenshot path, transcript)

## Related issues

Closes #
Closes #

<!-- One line per issue this feature/bug completes. Red tests, fixes and docs
     for the same feature are commits here, not separate PRs. -->

## Verification

- [ ] `make verify-quick` passes (the static gate set CI runs; pre-push)
- [ ] `make verify` passes, or the final CI run is green on this commit
- [ ] `make doctor` is clean, or the local/CI toolchain difference is noted
- [ ] Coverage checked when production lines changed (`make coverage BASE=…`)

## Review

- [ ] Feature/bug PR: reviewed by the maintainer and recorded below
      (`Reviewed-by: @…`)
- [ ] Docs/dependency-only PR: gate-only (state why)

## Red → Green sequence

- [ ] Each task in this feature has a red (failing) commit before its green fix
- [ ] A production change touches `tests/`, or this PR carries `tests-allow: <reason>`
- [ ] Or explain why not: ________________________________

## Checklist

- [ ] **One completed feature/bug, not one task**: the issues above are the whole change
- [ ] SOLID: no new SRP violations, dependency direction correct
- [ ] Hexagonal: `lib/` does not include from `tui/`, `src/`, or `tools/`
- [ ] Size limits: classes ≤200 lines, methods ≤10 lines with minimal branching
- [ ] Tests cover the new behaviour (≥80% line coverage for new code paths)
- [ ] `noexcept` on accessors and pure functions where applicable
- [ ] No dead code, commented-out code, or stubs
- [ ] No SPDX/copyright boilerplate; the first line is functional
- [ ] No new `std::thread::detach()` usage

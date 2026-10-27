## Summary

Describe the **feature** this PR delivers and why. Link the architecture
proposal (the epic, a `docs/fix-tracker.md` group, or an inline spec). One PR
delivers one feature; the tasks from its issue tree are commits on this branch,
not separate PRs.

## Related issues

Closes #
Closes #

<!-- One line per task this feature completes. Do not open a PR per issue:
     red tests, fixes and docs for the same feature are commits here. -->

## Verification

- [ ] `make clean && make` succeeds
- [ ] `make test` passes
- [ ] `make lint` — zero new clang-tidy warnings
- [ ] `make analyze` — zero new cppcheck warnings
- [ ] Manually verified (describe how)

## Red → Green sequence

- [ ] Each task in this feature has a red (failing) commit before its green fix
- [ ] Or explain why not: ________________________________

## Checklist

- [ ] **One feature, not one task**: the issues above are the whole feature
- [ ] SOLID: no new SRP violations, dependency direction correct
- [ ] Hexagonal: `lib/` does not include from `tui/`, `src/`, or `tools/`
- [ ] Size limits: classes ≤200 lines, methods ≤10 lines with minimal branching
- [ ] Tests cover the new behaviour (≥80% line coverage for new code paths)
- [ ] `noexcept` on accessors and pure functions where applicable
- [ ] No dead code, commented-out code, or stubs
- [ ] No SPDX/copyright boilerplate; the first line is functional
- [ ] No new `std::thread::detach()` usage

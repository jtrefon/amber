## Summary

Describe what this PR changes and why. Link to the architecture proposal
(`docs/fix-tracker.md` task or inline spec).

## Related issues

Closes #

## Verification

- [ ] `make clean && make` succeeds
- [ ] `make test` passes
- [ ] `make lint` — zero new clang-tidy warnings
- [ ] `make analyze` — zero new cppcheck warnings
- [ ] Manually verified (describe how)

## Red → Green sequence

- [ ] This branch contains a red (failing) commit followed by a green (fix) commit
- [ ] Or explain why not: ________________________________

## Checklist

- [ ] SOLID: no new SRP violations, dependency direction correct
- [ ] Hexagonal: `lib/` does not include from `tui/`, `src/`, or `tools/`
- [ ] Size limits: classes ≤200 lines, methods ≤10 lines with minimal branching
- [ ] Tests cover the new behaviour (≥80% line coverage for new code paths)
- [ ] `noexcept` on accessors and pure functions where applicable
- [ ] No dead code, commented-out code, or stubs
- [ ] New source files carry SPDX/Apache-2.0 header
- [ ] No new `std::thread::detach()` usage

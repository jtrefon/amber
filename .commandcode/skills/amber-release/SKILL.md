---
name: amber-release
description: Run the amber repo's change-to-release routine end to end — fix or feature on a branch, local + CI gates, push, PR, merge, then cut a release with the correct version bump (bugfix → patch, feature → minor) and sync the Homebrew tap. Use when the user asks to fix something and then "push, PR, bump version so I can test", or to cut a release for the amber repo.
argument-hint: "[fix|feature]"
---

# Amber change-to-release routine

Applies to the repo at `~/Projects/amber` (C++ agent harness). The user's
default expectation: every completed change is pushed, opened as a PR,
merged only when CI is green, and shipped as a brew-installable release with
the version bump dictated by the change type.

## 1. Versioning policy (hard rule)

`version.txt` and `homebrew/amber.rb` must always hold the SAME version.

- **Bug fix** → bump **patch** (`0.4.10` → `0.4.11`).
- **New feature** → bump **minor**, reset patch to 0 (`0.4.9` → `0.5.0`).
- **No major bumps** for now. A release window containing both a feature and
  fixes ships as a minor bump.
- Chore/docs/CI-only changes still get a patch bump when released (they must
  reach brew), unless they are purely internal to a release in flight.

## 2. Change loop (before any release talk)

1. Branch off `main`: `<type>/<short-desc>` with `type` in
   `fix|feat|chore|docs|refactor|release`.
2. Investigate first; per AGENTS.md a bug fix starts with a failing test
   (Red → Green). Add regression tests for every fixed behavior.
3. Local gates before pushing:
   - `make test` — the 100+ unit gate.
   - `./run_tests` — extended binary (unit + agent-loop + TUI logic).
   - `make check` — build hygiene. **If `tests/run_tests.cpp` (or any
     audited file) grew, sync the line-count table in AGENTS.md** — the
     hygiene gate fails otherwise ("P5: audit table lists ... lines; tree
     has ..."). Same for `tui/tui_input.cpp` when touched.
   - clang-tidy/cppcheck are NOT installed locally; lint/analyze run only in
     CI — never claim them green locally.
4. Commit: imperative, scoped prefix (e.g. `fix: replayed tool_calls …`),
   heredoc body, and the mandatory trailer line
   `Co-authored-by: CommandCodeBot <noreply@commandcode.ai>`.
5. Push, open the PR with a body that states root cause, fix, tests, and
   verification. Watch `gh pr checks <n> --watch` — the 15 checks include
   lint (~25-28 min), analyze, g++/clang++/macOS-arm64 build-and-test,
   format-check, check. `--watch` exits early sometimes; re-run it or poll
   `gh pr checks`.
6. Squash-merge with `--delete-branch` only when ALL checks pass. Sync main.

## 3. Release flow

Run only after merged content sits on `main` and you know what it contains
since the last tag (`git log --oneline <last-tag>..origin/main`):

1. Determine the bump from section 1 (features present? minor, else patch).
2. `git checkout -b release/vX.Y.Z` from main; set the version in
   `version.txt` AND `homebrew/amber.rb` (`version "X.Y.Z"`); commit
   `release: bump version to X.Y.Z` describing the contents.
3. Open the release PR (release/version-only). Wait for the full 15-check
   green (it re-runs everything) then squash-merge and delete the branch.
4. Sync main, then `git tag -a vX.Y.Z -m "…"` and `git push origin vX.Y.Z`.
   The tag triggers `.github/workflows/release.yml`.
5. Watch the release workflow run. It builds the **darwin-arm64 tarball only**
   (Intel macOS was retired) plus Linux packages, then publishes the GitHub
   Release. Verify `gh release view vX.Y.Z --json assets` contains
   `amber-<ver>-darwin-arm64.tar.gz` + `.sha256`.
6. Trigger the tap sync:
   `gh workflow run bump.yml --repo jtrefon/homebrew-amber`, then verify the
   formula: `gh api repos/jtrefon/homebrew-amber/contents/Formula/amber-agent.rb`
   shows `version "X.Y.Z"` (arm-only formula, `on_arm` sha, no on_intel).
7. Final check: `brew update --quiet` then
   `brew info jtrefon/amber/amber-agent` must show `old → stable X.Y.Z`.

The user then tests with:
```
brew update
brew upgrade jtrefon/amber/amber-agent
```

## 4. Pitfalls learned (check before cutting a release)

- **AGENTS.md audit table**: every release touches `tests/run_tests.cpp`
  counts; sync them in the same PR that adds tests, or `make check` fails.
- **kilocode/gateway quirks**: kilo's `/models` advertises `context_length`
  (not `n_ctx`) and lists `kilo-auto/frontier` (1M) before `kilo-auto/free`
  (256k) — the probe must pass the explicit active model. Replayed history is
  sanitized at the wire (`sanitize_tool_calls`). The kilo balance readout
  resolves its token from `kilo_balance_token` override, else the kilocode
  provider's `api_key` (the key the TUI prompt stores).
- **GitHub Actions quirks**: `gh run watch` on multi-job workflows can exit
  while jobs queue; poll `gh pr checks`. `gh release view latest` is broken
  on gh ≥ 2.100 ("release not found") — the tap bump uses the REST
  `releases/latest` endpoint instead.
- **Never** hand-edit `include/agent/version.h` (generated by configure) and
  never touch the local inference service on :8081.

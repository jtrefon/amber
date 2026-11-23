# Release checklist

Run this in order. Every command must succeed before the next step.

## 1. The release commit

- [ ] `version.txt` is stamped with the release version (`X.Y.Z`).
- [ ] `debian/changelog` and both `rpm/*.spec` versions are either updated or
      left to the CI stamp; verify the CI stamp still matches `version.txt`.
- [ ] `homebrew/amber.rb` has the release version. The sha256 is bumped after
      the macOS tarball exists.
- [ ] `python3 tools/release_check.py --tag vX.Y.Z --check-formula` passes.
- [ ] `SECURITY.md`, `docs/UPGRADING.md` and `docs/release-checklist.md` are in
      the tree.

## 2. Local gates

- [ ] `make preflight` (or `make verify` plus `make check-compilers` when the
      journey suites cannot run here; `make verify-container` when the pinned
      analyzers are not installed).
- [ ] `make coverage BASE=origin/main` (instrumented build; restore afterwards
      with `./configure && make clean && make`).
- [ ] `make test` includes the pty suite and the plugin conformance tests.
- [ ] `make check` and `make verify-quick` are clean.
- [ ] `make upgrade-check UPGRADE_OLD=<0.8.9>/usr/local/bin/amber` passes on the
      release candidate; record the output and the 0.8.9 tarball name.
- [ ] `make journey-matrix-release` is green: no `deferred:` cell remains.
- [ ] Every manual cell claimed for 0.9 in `docs/platform-support.md` has a
      recorded result in #698 (Terminal.app, iTerm2, GNU screen, plain ssh,
      WSL2 and any other named xterm-256color emulator); unrecorded cells are
      named as unverified in the release notes.
- [ ] `make docs`/website build and smoke if the site changed.

## 3. Tag

- [ ] `git tag vX.Y.Z` on the release commit.
- [ ] `git push origin vX.Y.Z`.
- [ ] `release.yml` builds, tests, stages `make install`, verifies the licence
      files, attests and signs the artifacts, and publishes the GitHub release.
- [ ] `packages.yml` builds the `.deb`/`.rpm`, signs them, verifies a fresh
      install from the repository, and publishes the apt/dnf indexes.
- [ ] Confirm the release is not a pre-release unless the tag has a suffix.

## 4. Homebrew

- [ ] Download `amber-X.Y.Z-darwin-arm64.tar.gz` and its checksum.
- [ ] Bump `homebrew/amber.rb`'s `sha256` and mirror the change to the
      `jtrefon/homebrew-amber` tap.
- [ ] `brew install jtrefon/homebrew-amber/amber-agent` in a clean shell, then
      `amber-cli --version` and a bootstrap run from an empty directory.
- [ ] Verify `share/doc/amber-agent` contains `LICENSE`, `NOTICE` and
      `THIRD_PARTY_LICENSES`.

## 5. Website and announcement

- [ ] The website version reads the tagged `version.txt` and the build/smoke
      jobs are green.
- [ ] Beta copy is replaced with the 1.0 status.
- [ ] Release notes list breaking changes, the plugin tier status and the
      coverage/floors actually measured.
- [ ] The GitHub release, apt/dnf repositories and Homebrew tap all report the
      same version.

## 6. Post-release

- [ ] `amber update` on each channel reports the new version.
- [ ] A clean container installs from apt and dnf.
- [ ] A 0.8 -> 1.0 upgrade is exercised once, including plugin state.
- [ ] The 1.0 milestone is empty and `ci-gate` is green on `main`.

# Linux distribution and update channels

amber ships to macOS through a Homebrew formula and to Linux through a
hand-attached `.deb`/`.rpm` built by `fpm`. Neither channel is complete: the
Linux packages cannot be installed from anywhere, cannot be updated by anything,
and install several files where the FHS says they do not belong. This document
plans the correction in two milestones.

Everything below is a decision record, not a proposal for discussion. The four
choices that cannot be revisited once published are marked **locked** with the
evidence that settled them.

## Constraints

- GitHub stays the home of the project. No hosting migration.
- The native package manager owns updates. amber does not run an updater.
- Runtime, development payload and bench harness are separate packages.
- Source packages ship where a native channel requires one (`.dsc` set,
  `.src.rpm`, `PKGBUILD`).
- WSL2 Ubuntu is a primary path and consumes the same `.deb` as bare metal.
- Current, supported distros only. Ubuntu is primary, Debian is deferred, one
  RPM distro is enough.
- macOS stays on Homebrew. amber never writes into the Cellar.

## Locked decisions

| Decision | Value | Why this one |
|---|---|---|
| Package family | **`amber-agent`** | `amber` is taken in Debian (`0.0~git20171010.cdade1c-2`) and in npm (the Smalltalk implementation) and PyPI; `amber-ai` is crowded by `amber.ai` / `amberai.ai`; `amber-cli` is taken in openSUSE. `amber-agent` is free in Debian, Ubuntu, Fedora, PyPI and npm. `amber-harness` is the runner-up and still free if this is ever revisited. |
| Repository host | **`jtrefon.github.io/amber-packages`** | A second Pages site in its own repo, so the website stays on `jtrefon/amber`. No custom domain: accepted residual risk, mitigated below. |
| APT shape | **one `stable` suite** | A codename never appears in a user's `sources.list`, so a distro upgrade cannot 404 the repository. APT cannot expand `$(VERSION_CODENAME)`, and baking one in goes stale on upgrade. GitHub CLI ships this exact shape. |
| RPM shape | **one repo at `rpm/fedora/$basearch/`** | Same reasoning; `$releasever` stays out of the URL so a future Fedora is not a 404. |
| Architecture | `amd64` first | Intel macOS was retired for the same reason: one architecture done properly beats two done halfway. |
| Signing | dedicated RSA-4096 sign-only key, 10 years, generated offline | Signing `InRelease`, `repomd.xml` and every RPM. `.deb` payload signatures are not a thing and will not be invented. |

### What the rename costs

Nothing is published yet, so the cost is zero now and permanent later. The
binaries keep their names (`amber`, `amber-cli`, `amber-bench`); only the package
is `amber-agent`. A package name that differs from the command it installs is
ordinary in Debian (`ripgrep` installs `rg`).

## What is broken today

Measured against `Makefile.in`, `configure`, `lib/`, `tui/` and `bench/` on
`7d6e260`. Each row is a defect, not a preference.

| # | Defect | Evidence |
|---|---|---|
| 1 | `install` builds one undifferentiated tree: binaries, static libs, headers, prompts, plugins and the bench harness all land together | `Makefile.in` `install:` target |
| 2 | Headers collide with the system: `/usr/include/agent` and `/usr/include/nlohmann` | `install -d $(DESTDIR)$(includedir)/nlohmann` |
| 3 | Plugin binaries go to `/usr/share/amber/plugins`, which is architecture-independent data | `install -m 755 $(CDP_PLUGIN) $(DESTDIR)$(datadir)/plugins/cdp/` |
| 4 | `keybindings.json` exists in the repo but is never installed; the TUI silently falls back | `tui/tui.cpp:225`, absent from the install target |
| 5 | `data/project-signals.json`, `data/skill-collections.json`, `data/skill-owners.json` are never installed | `tui/tui_input.cpp:863-870` |
| 6 | `amber-bench` is installed but its scenarios and profiles are not; it resolves both from `fs::current_path()` | `bench/main.cpp:65`, `bench/main.cpp:124` |
| 7 | `LDFLAGS` is never referenced, so a distro cannot inject hardening flags | zero occurrences in `Makefile.in` |
| 8 | `SOURCE_DATE_EPOCH` is ignored, so packages are not reproducible | absent from `configure` and `Makefile.in` |
| 9 | No `--libexecdir` or `--mandir`; no man pages, no shell completions, no pkg-config file | `configure` option list |

Defects 4-6 mean the installed product is visibly degraded compared with a
source checkout, which no packaging change can paper over.

A tenth defect — `configure` and `GNUmakefile` declaring GPL-3.0-or-later while
`LICENSE` is Apache-2.0 — was found while writing this plan, ruled a mistake
rather than a decision, and fixed before M1 started. The project is
Apache-2.0-only and no file claims otherwise.

## M1 — packaging conformance

**Problem.** The tree is installable by `fpm` only because `fpm` copies whatever
`make install` produces. Nothing about that output conforms to a distro's
expectations, and three runtime data files are missing from it.

**What ships.**

1. `configure` gains `--libexecdir` (default `$(prefix)/libexec`), `--mandir`,
   `--with-system-pkgconfigdir`-free pkg-config generation, and honours
   `LDFLAGS`. `--help` documents each.
2. `Makefile.in` `install` is decomposed into `install-runtime`, `install-dev`,
   `install-bench` and `install-data`. `install` remains their union so the
   macOS staging tree and the Homebrew formula keep working unchanged.
3. Install layout becomes:

   | Path | Package |
   |---|---|
   | `/usr/bin/{amber,amber-cli}` | `amber-agent` |
   | `/usr/share/amber/{prompts,completions.json,keybindings.json,data}` | `amber-agent` |
   | `/usr/lib/amber/plugins` (Debian) / `/usr/libexec/amber/plugins` (Fedora) | `amber-agent` |
   | `/usr/share/doc/amber-agent/{LICENSE,NOTICE,THIRD_PARTY_LICENSES}` | `amber-agent` |
   | `/usr/lib/amber-agent/*.a`, `/usr/include/amber-agent/{agent,nlohmann}`, `amber-agent.pc` | `amber-agent-dev` / `-devel` |
   | `/usr/bin/amber-bench`, `/usr/share/amber-bench/{scenarios,profiles.json}` | `amber-agent-bench` |

4. Plugin discovery searches the compiled `AMBER_PLUGIN_DIR` first and keeps
   `<exe>/../share/amber/plugins` and `/usr/share/amber/plugins` as fallbacks,
   so existing installs keep working through the move.
5. `amber-bench` resolves its data root from the compiled `AMBER_BENCH_DATA_DIR`
   and falls back to `./bench`, so a source checkout and an installed package
   both work.
6. `debian/` (native, `3.0 (quilt)`) and `rpm/amber.spec` replace `fpm`. Both
   build from one deterministic `git archive` tarball
   (`SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)`, sorted entries, no owner
   metadata), so `.dsc` and `.src.rpm` are regenerable from the tag.
7. `packaging/arch/PKGBUILD` is refreshed from `pkgver=0.3.1` and consumes the
   same tarball.
8. A `packaging` CI job builds the `.deb` set, the RPM set, the `.dsc` and the
   `.src.rpm`; `lintian` and `rpmlint` gate them.
9. An `install-verify` job installs the produced packages in `noble`,
   `resolute`, `stonking`, `fedora:44` and `fedora:45` containers, then runs
   `amber --version`, `amber-bench list`, and compiles one translation unit
   against the `-dev` package using `pkg-config --cflags`.

Step 9 is what makes the single-`stable`-suite decision safe: one build, five
proofs. If a release fails verification on one distro, the answer is to split
that distro's suite, not to weaken the check.

**Out of scope.** No repository, no signing, no `amber update`. No arm64. No
Debian, openSUSE, Arch AUR or Flatpak. No change to how the binaries behave.

**Done when.** A tag produces `.deb`, `.rpm`, `.dsc` and `.src.rpm` as release
assets; every package installs and runs on all five target distros; `lintian`
and `rpmlint` are clean; `amber-bench list` finds scenarios from an installed
package outside the source tree.

## M2 — signed channels

**Problem.** M1 produces files nobody can install from, and nothing verifies
them. A package that arrives over the network unsigned is a download, not a
channel.

**What ships.**

1. Key ceremony: an RSA-4096 sign-only key, 10-year expiry, generated offline,
   whose private key exists only in the `amber-packages` repository's `release`
   environment (required reviewers) as an encrypted secret. The public key and
   its fingerprint are committed under `packaging/keys/`, installed by
   `amber-agent-release`, and readable locally with
   `gpg --show-keys --with-fingerprint /usr/share/keyrings/amber-agent-archive-keyring.gpg`.
   Verification is a documented command, not a new flag.

   The key exists: `amber-agent archive signing key <hello@trefon.com>`,
   fingerprint `C373A7C2F4BB30ABEB1B35DAAAFCEAE6C29E0D1B`, expires 2036-10-27.
   Signing material lives in `jtrefon/amber-packages` → environment `release`,
   restricted to `main`, as `GPG_PRIVATE_KEY` and `GPG_PASSPHRASE`, with the
   fingerprint in the `GPG_KEY_ID` variable.
2. `amber-agent-release` ships the trust anchor and the source definition:

   ```
   /usr/share/keyrings/amber-agent-archive-keyring.gpg
   /etc/apt/sources.list.d/amber-agent.sources   # deb822, Signed-By:
   ```

   and on RPM:

   ```
   /etc/pki/rpm-gpg/RPM-GPG-KEY-amber-agent
   /etc/yum.repos.d/amber-agent.repo             # gpgcheck=1, repo_gpgcheck=1
   ```

   Fedora 45 moved **Fedora's own** repository files to `/usr/share/dnf5/repos.d`;
   third-party repositories still belong in `/etc/yum.repos.d`, which is where
   this one goes.
3. Publishing: `.github/workflows/packages.yml` regenerates
   `apt/dists/stable/{Release,InRelease,Packages,Packages.gz}` and
   `rpm/fedora/x86_64/repodata/`, signs `InRelease` and `repomd.xml`, signs every
   RPM, and pushes to `jtrefon/amber-packages` with a `.nojekyll` at the root.
4. Retention: the current release plus the previous two stay in the pool;
   anything older is archived to GitHub Releases. At roughly 1.5 MB per package
   this holds the published site near 30 MB against Pages' 1 GB ceiling.
5. Verification is two-sided. Positive: a container installs from the repository
   with `apt-get update` and `dnf install`. Negative: the same install against a
   tampered `InRelease` and against a removed `Signed-By` **must fail**. The
   negative test is the point — a signature nobody checks is decoration.
6. `amber update` reads `/etc/os-release`, and either prints the exact command or,
   on an interactive TTY with an explicit confirmation, runs
   `apt-get update && apt-get install --only-upgrade amber-agent` / `dnf upgrade`.
   It never passes `--yes`, never runs unattended, and on Homebrew it only points
   at `brew upgrade`. Non-interactive callers get the command and a non-zero exit.
7. Migration from `fpm`: the `.deb` carries `Conflicts: amber (>= 0.1.0)` and
   `Replaces: amber (>= 0.1.0)`; the RPM carries `Obsoletes: amber < 0.9`. The
   version floor is what keeps this safe — Debian's `amber` is `0.0~git…`, below
   the floor, and no Ubuntu series ships `amber` at all, so only the old `fpm`
   packages match.
8. Documentation: an install page with the three commands, the fingerprint, and
   an explicit statement that the repository is `stable` rather than per-codename.

**Out of scope.** No in-app background updater, no update manifest, no delta
patches (split packages already cut the update size), no auto-merge into distro
archives.

**Done when.** A clean container installs `amber-agent` from the repository with
a signed chain, `apt-get update` fails against a tampered index, and a user on
the old `fpm` package upgrades in one `apt install`.

## Residual risks

| Risk | Mitigation |
|---|---|
| The Pages hostname is permanent in every user's `sources.list` | If it must ever move, publish one final `amber-agent-release` from the **old** URL that rewrites the source file; `amber update` can also repair it. This is why the release package, not the docs, owns the source file. |
| Signing key loss | Two offline copies of the private key; the public key is also in git, so a re-key ships as a new keyring package signed by the old repository while it still exists. |
| A future Ubuntu drops `libncursesw6` | `install-verify` fails on that distro and the suite splits per-codename then. Building per-codename now would pay that cost for every release to hedge a risk one CI job already detects. |
| Fedora 45 is not released yet (GA 3 Nov 2026) | It joins the verification matrix at GA, not before. Fedora 43 leaves the matrix at its 2026-12-09 EOL. |
| `amber-bench` scenarios drift from `bench/profiles.json` | The bench package pins both; the scenario tests already read them from the tree. |

## Found while planning, fix inside M1

- `homebrew/amber.rb` pins `0.7.0` while `version.txt` is `0.8.0`. A release that
  ships a stale formula is a release with two versions.
- `docdir` is `$(datarootdir)/doc/amber`; Debian expects
  `/usr/share/doc/amber-agent`. Same content, different owner.

## Non-goals

Flatpak, Snap, AppImage, Nix, arm64, rpm-ostree layering, Debian, openSUSE,
official distro-archive inclusion. Inclusion is a human-reviewed goal for later
and gets no automated path — a packaging pipeline that can land itself in Debian
is a pipeline nobody reviewed.

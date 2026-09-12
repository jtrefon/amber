# Contributing to amber

Thanks for your interest in improving amber! This document explains how to
build, test, and submit changes.

## Ground rules

- Be respectful. All participation is governed by our
  [Code of Conduct](CODE_OF_CONDUCT.md).
- By contributing, you agree that your contributions are licensed under the
  project's [Apache License 2.0](LICENSE).

## Getting started

Requirements (Debian/Ubuntu names shown):

- A C++17 compiler (`g++` or `clang++`)
- `libcurl4-openssl-dev`
- `libncursesw5-dev` (wide-character ncurses, for the TUI)
- `pkg-config` and `make`

```sh
sudo apt-get install -y build-essential libcurl4-openssl-dev \
    libncursesw5-dev pkg-config
```

## Build and test

```sh
./configure          # detects compiler, libcurl, ncursesw
make                 # builds libagent_core.a, libagent_tools.a, amber, amber-cli
make test            # runs the unit test suite
```

The bootstrap `GNUmakefile` will run `./configure` for you if you just run
`make`. Both binaries and `make test` must succeed before a change is merged;
CI runs the same steps on `g++` and `clang++`.

## Coding style

- Follow the existing style; a `.clang-format` is provided. Run
  `clang-format -i <files>` on code you touch.
- C++17. Four-space indentation, no tabs, 100-column soft limit.
- Keep the core library (`lib/`, `include/agent/`) free of UI concerns. UI code
  lives in `tui/` and depends on the library through `AgentHooks`, never the
  reverse.
- Do not add comments that merely restate the code.
- New source files carry no copyright or SPDX header, keep the first line
  functional, matching the rest of the tree.

## Writing a plugin

amber is extensible through a plugin framework. Two tiers exist, pick
deliberately:

- **Core plugin**: C++17, compiled into amber. Can contribute tools, LLM
  providers, prompt blocks, status segments, panels, wallets and allowances, and
  subscribe to typed events.
- **External plugin**: a separate executable speaking JSON-RPC over stdio.
  Tools only, but crash-isolated and language-agnostic.

Start with the **plugin developer guide**:
[`docs/spec/plugins/developer-guide.md`](docs/spec/plugins/developer-guide.md).
It has a quickstart, a tier decision tree, the capability catalogue, and the
availability table that is the single status surface for what ships today. A
complete, compiled, tested example lives in
[`examples/plugin_hello/`](examples/plugin_hello/), exercised by
`tests/example_plugin_test.cpp`.

## Submitting changes

1. Fork and create a topic branch.
2. Make focused commits with clear messages (imperative mood, e.g.
   "tui: fix drawer scroll").
3. Add or update tests in `tests/run_tests.cpp` for behavior changes.
4. Ensure `make && make test` pass locally.
5. Open a pull request describing the change and how you verified it.

## Releasing (maintainers)

Releases are tag-driven. CI (`.github/workflows/release.yml`) runs when a semver
tag is pushed:

```sh
git tag v0.1.0          # or v0.1.0-rc.1 for a pre-release
git push origin v0.1.0
```

The workflow builds and tests, stages `make install`, then publishes a GitHub
Release with:

- `amber-<version>-linux-x86_64.tar.gz` (+ `.sha256`)
- `amber_<version>_amd64.deb` (+ `.sha256`)
- `amber_<version>.x86_64.rpm` (+ `.sha256`)
- `amber-<version>-darwin-arm64.tar.gz` (+ `.sha256`), macOS Apple Silicon
- the optional CDP browser plugin packages (`.deb` / `.rpm`)
- auto-generated release notes

Tags containing a hyphen (e.g. `v0.1.0-rc.1`) are marked as pre-releases.
Follow [semantic versioning](https://semver.org/): patch for fixes, minor for
backward-compatible features, major for breaking changes.

## Reporting bugs and requesting features

Use the [report page](https://jtrefon.github.io/amber/report/) or open an issue
directly, the [issue forms](.github/ISSUE_TEMPLATE/) pre-fill the details we
need. For security issues, please see the security notes in the README rather
than filing a public issue.

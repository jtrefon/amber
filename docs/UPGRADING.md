# Upgrading amber to 1.0

This note covers the move from the 0.8 line to 1.0. It is deliberately short;
the release notes carry the per-release details.

## 0.8.9 -> 0.9.0

0.9 adds named profiles and per-turn checkpoints. On the first 0.9 start, when
the profiles directory does not exist, the connection the config files resolved
is saved as `profiles/default.json` (mode 0600). Sessions, the global config,
the project `amber.conf` and plugin state keep their paths; a plugin disabled
in 0.8.9 stays disabled.

The installed-tree check:

```sh
# Extract amber-0.8.9-<platform>.tar.gz, build the candidate, then:
make upgrade-check UPGRADE_OLD=/path/to/amber-0.8.9/usr/local/bin/amber
```

It drives the real 0.8.9 TUI and the candidate against one scratch XDG tree and
project, a loopback endpoint and a real git repository. It asserts:

- the 0.8.9 session's context is carried into 0.9;
- `profiles/default.json` has the migrated endpoint, model and key (0600);
- a second profile and `<project>/.amber/profile` pin apply before the turn;
- the profile key is masked in the transcript;
- the checkpoint turn creates a ref under `refs/amber/checkpoints`, the history
  survives a restart, and the user's HEAD/branch do not move;
- the 0.8.9 plugin state survives;
- a missing config falls back with `info: wrote default config`, and a bad
  legacy value warns (`config: ... using default`) instead of crashing.

The 0.9.0 package/Homebrew upgrade itself is P4: it uses the same state, and
`amber-cli update` selects the system package manager. The check uses the
previous release tarball and the candidate build because the 0.9.0 package does
not exist before the tag.

Observed 2026-11-23 on macOS arm64 with
`amber-0.8.9-darwin-arm64.tar.gz` and the current `main`: all checks passed
(the script prints each).

## Packages

- The package is named `amber-agent` on apt and dnf. The binaries remain
  `amber`, `amber-cli` and `amber-bench`.
- Homebrew users should keep using the fully qualified
  `jtrefon/homebrew-amber/amber-agent`; bare `amber` is a different project.
- Pre-1.0 packages named `amber` are replaced/obsoleted by `amber-agent` at the
  documented version floor.

## Config and state

- Global config remains under `$XDG_CONFIG_HOME/amber` (or
  `~/.config/amber`).
- Per-project private state remains under
  `$XDG_STATE_HOME/amber/projects/<slug>`; shareable project config under
  `<project>/.amber`.
- Legacy state is adopted once by `Workspace::adopt_legacy_state()`; the
  original is not deleted.
- Secrets and state files are owner-only (`0600`). If an older install left
  them world-readable, the next write narrows the file; a one-off
  `chmod 600` on copied backups is still a sensible precaution.

## Plugins

- Bundled plugins are enabled by default. `/get plugin list` reports state and
  contributions; `/set plugin on|off <id>` is the only write path.
- The external (process) tier is **tools-only** in 1.0. External plugins can
  contribute tools and manifest settings; command namespaces from an external
  manifest are not mounted.
- External plugin tools are owner-scoped and unwind exactly with their plugin.
  A process starts lazily on the first tool call, so enabling a plugin does not
  block the UI.
- Core plugins compile against the headers in `include/agent/`. The 1.0
  compatibility promise is in
  [`docs/spec/plugins/api-compatibility.md`](spec/plugins/api-compatibility.md).

## Behaviour changes

- A plugin that throws during declaration, activation, install or shutdown is
  contained and reported as a failed plugin; the harness keeps running.
- Plugin activation follows registration order, not plugin-id order.
- The TUI no longer merges external plugin command subtrees. Use the plugin's
  tools or ask the plugin author to ship a bundled command capability.
- Codecov is no longer the authoritative coverage surface; the in-repo
  coverage recipe is. See the coverage note in the README.

## Rolling back

A 1.0 install can be replaced by the previous 0.8 package with the package
manager's normal downgrade path. Plugin state files survive; a plugin id or
state path removed in 1.0 is not automatically migrated back.

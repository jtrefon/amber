# Upgrading amber to 1.0

This note covers the move from the 0.8 line to 1.0. It is deliberately short;
the release notes carry the per-release details.

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

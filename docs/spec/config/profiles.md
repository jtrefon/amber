# Spec: Named profiles

A **profile** is a named provider connection: the account, endpoint and model a
consultant or a multi-project developer switches between. Switching a profile
changes the connection and nothing else — mode, UI settings, tools, project
state and the conversation stay where they are.

## Ownership

| Piece | Where |
|---|---|
| Store, validation, masking, project file | `lib/profiles.cpp`, `include/agent/profiles.h` |
| Command surface | `completions.json` (`profile.*`), `tui/profile_commands.cpp` |
| Status bar | `StatusSegment::model`, `StatusSnapshot::profile` |
| Startup + project attach | `tui/tui_main.cpp`, `tui/slash_dispatcher.cpp` |
| Tests | `tests/profiles_test.cpp`, `tests/tui_pty_test.cpp`, `tests/status_segments_test.cpp` |

## What a profile carries

`provider_name`, `flavor`, `api_base`, `api_key`, `model`, `temperature`,
`max_tokens`, `thinking`. Nothing else. Tool permissions, agent mode, UI
settings and wallets stay global: a profile is an account, not a workspace.

A profile does **not** carry `context_size`. A context window is measured
against one model and one endpoint, so applying a profile clears
`context_size`/`context_explicit` and lets auto-detection resolve the new
model. Carrying a pin across is how the gauge, the compression gate and the
429-kill budget kept lying after a switch.

## Storage

```
~/.config/amber/profiles/<name>.json     one file per profile, mode 0600
~/.config/amber/profiles/                directory, mode 0700
<project>/.amber/profile                 one line: the profile name
```

The directory is `global_config_dir()/profiles`, so `XDG_CONFIG_HOME` and
`/get config paths` agree with the writer.

JSON shape:

```json
{
  "version": 1,
  "name": "work",
  "provider": "openrouter",
  "flavor": "openai",
  "api_base": "https://openrouter.ai/api/v1",
  "api_key": "sk-...",
  "model": "anthropic/claude-sonnet-4",
  "temperature": 0.2,
  "max_tokens": 16384,
  "thinking": "auto"
}
```

A malformed file is skipped by `/profile list`; naming it explicitly reports
that it could not be loaded. Writes are atomic (`write_private_file`) and the
file is created 0600, never chmod-after-write.

### Names

`[A-Za-z0-9._-]+`, at most 64 characters, never `.` or `..`. The name is a
file stem; a name that can escape the profiles directory is refused before it
reaches a path.

### Keys

A key is never printed in full. `mask_key` returns the first three and last
four characters (`sk-...abcd`), `****` for a key of eight characters or fewer,
and `(unset)` for an empty key. `/profile`, `/profile list`, `/profile show`
and `/profile show <name>` all use it. The masked form is also what the
provider and global-config readouts should use; a full key exists only in the
profile file and the live connection.

## Migration

On the first TUI/CLI start where `profiles/` does not exist, the connection the
**config files** resolved is saved as `default`: the global selection, the
project `amber.conf` and an explicit `--config`. The order is deliberate —
migration runs before `AMBER_*` overrides and before the project pin:

- An `AMBER_API_KEY` is a per-invocation override, so writing it into
  `~/.config/amber/profiles/default.json` would persist a credential the user
  chose to keep out of a file.
- `<project>/.amber/profile` selects an account for that project; it is not the
  user's default account.

An existing profiles directory is never touched, so a deliberately edited
`default` survives later starts. A user who removes `default` and keeps another
profile is not re-migrated.

`profile=` is written to `~/.config/amber/config` only when the active profile
is not `default`; the absent key means `default`, which keeps the global file
minimal.

## Precedence

The profile supplies the connection after the provider selection and before
per-invocation overrides. In the TUI:

```
env vars
  > <project>/.amber/profile      (the project's pin)
  > <project>/amber.conf / settings
  > global profile (profile= in ~/.config/amber/config)
  > global provider selection
  > defaults
```

The CLI applies the global profile the same way, but does not apply a project's
`.amber/profile`: `--model`, `--api-base` and `AMBER_*` are per-invocation
overrides, and a project pin is a window/host concern. A global `model=` does
not overwrite an active profile's model (`GlobalConfig::profile_applied`), so a
stale global model cannot silently replace the account's model.

A profile's `api_base` sets `api_base_from_provider`, so a workspace
`amber.conf` cannot redirect the endpoint it names.

Only two layers are profile selectors: `profile=` in
`~/.config/amber/config`, and `<project>/.amber/profile`. A `profile=` key in a
project `amber.conf`, the private settings file or a `--config` file is parsed
but restored to the selected value, because those layers do not apply the
profile's connection — accepting the name alone would leave the name and the
connection disagreeing.

## Commands

| Command | Behaviour |
|---|---|
| `/profile` | Show the active connection: provider/flavor, endpoint, model, masked key, sampling. |
| `/profile list` | One row per profile, sorted; the active one is marked `>`. The active row appears even when its file is missing, with `(not saved)`. |
| `/profile add <name>` | Opens the shared connection form (server URL, key, model) seeded from the active connection, then saves a new profile. An existing name is refused. |
| `/profile save` | Writes the active connection into the active profile. This is how `/set model`, a temperature change or a key edit becomes part of the profile. |
| `/profile switch <name>` | Applies a profile. Busy active window: the switch is queued and lands at the run slot's next idle boundary. |
| `/profile show [name]` | Full details, masked. The active profile is read from the live connection; another profile from its file. |
| `/profile remove <name>` | Deletes the file. Removing the active non-default profile asks, switches to `default` first (creating it from the current connection if absent), then removes. Removing the active `default` is refused. |

Every name under `switch`, `show` and `remove` is a live completion leaf
generated from the store (`ProfileSurface::refresh_feed`), the same pattern as
models, providers and worktrees. No command path or choice list is hardcoded in
C++.

`/profile` is both a command (the readout) and a namespace (its verbs). The
drawer opens as the line is typed, and Enter accepts the highlighted row; send
the bare line with **Ctrl-G** ("send as typed", shown in the input hint and true
for every namespace) to run the readout itself. `/profile <verb>` with its
argument is unambiguous and needs no modifier.

## Switching semantics

- Only the connection changes. The tool registry, mode, UI settings and the
  conversation are untouched.
- A running turn keeps the client it started with: the active window's run slot
  is checked before the switch, and a busy window queues the switch. The queued
  switch is applied on the UI thread in `drain_pending_prompts` once the run
  slot reports idle, before any queued prompt is dispatched. Sibling windows
  keep their own agents.
- The provider service is rebuilt against the new connection, the global config
  is saved, the wallet is refreshed and the model/provider feeds are rebuilt by
  `land_provider_switch`.
- `Agent::set_provider` never runs while the window's worker is live; that is
  the race the old `/provider` refusal existed to avoid.
- The status bar shows `@<name>` inside the model cell for any non-`default`
  profile. The implicit default is omitted: on a single-account install it
  would spend bar width saying nothing, and `/profile` always answers.

## Project pin

`<project>/.amber/profile` contains one name (blank lines and `#` comments are
ignored). It is read:

- at TUI startup, after the project settings and before the environment, so
  every restored window starts on that connection;
- when `/project open` opens a window, so the new window's first turn uses it.

An invalid name or a missing profile is reported and the active connection is
kept; a project pin never leaves the session without a usable connection.

## Security

- Files are 0600 and the directory 0700; `secure_private_state` narrows an
  existing tree on startup.
- A profile name is validated before any path is built.
- The full key never appears in a status line, `/profile` readout, completion
  help or log. `mask_key` is the only formatter.

## Scenarios

- **P-01**: save/load round-trips every field; the file is 0600 and the
  directory 0700.
- **P-02**: a name with `/`, `..`, whitespace or >64 characters is refused.
- **P-03**: applying a profile replaces the connection, keeps mode/UI, and
  clears the context pin and model capabilities.
- **P-04**: a missing profiles directory migrates the current connection as
  `default` once; a later start does not overwrite an edited default.
- **P-05**: `profile=work` in the global config applies the work connection and
  a stale global model does not overwrite it.
- **P-06**: `<project>/.amber/profile` selects a profile at startup and on
  `/project open`; a bad name is reported and the connection is kept.
- **P-07**: `/profile list` shows masked keys; the pty journey switches
  profile, sees `@work` in the bar, and never prints the full key.
- **P-08**: a switch requested while the active window is busy is queued, the
  running turn completes, and the next turn uses the new connection.
- **P-09**: migration captures the config-file connection only; an
  `AMBER_API_KEY` present only in the environment never reaches `default.json`.

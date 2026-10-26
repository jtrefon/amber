# Skill acquisition

Find, inspect, install, update and remove third-party skills from inside amber.
This is the user guide for the feature specified in
`docs/spec/skills/skill-acquisition.md`.

The registry is [skills.sh](https://skills.sh); details are read from each
skill's GitHub repository. `skills.com` is not an agent-skill registry.

## Quick start

```text
/skills
```

The explorer opens. Press `/`, type at least two characters, and press Enter.
Move the selection with `j`/`k`; press Enter again to load the detail (source,
installs, license, category, file count). Press `i` to install.

```text
/skills installed
```

Opens the shelf of skills already on disk.

## Keys

| Key | Action |
|---|---|
| `/` | focus the search field |
| `j`/`k`, arrows | move |
| `Enter` | leave the search field / load the selected detail |
| `i` | install (asks project or global) |
| `6` | Installed shelf |
| `A` | adopt an unmanaged skill directory (adds provenance, rewrites nothing) |
| `x` | enable / disable through the catalog override |
| `b` | block |
| `u` | check for an update; press again to apply |
| `D` | remove (confirmation required) |
| `/skills installed` | open the Installed shelf directly |
| `q`, `Esc` | close |

Installing is atomic: files are staged beside the destination and renamed into
place only when every file is written. A failed install leaves nothing behind.

## Where skills live

- project scope: `<project>/.amber/skills/<name>/` (shareable, default)
- global scope: `~/.config/amber/skills/<name>/`
- provenance: `<skills root>/.lock.json` (id, source, ref, blob hash, local hash,
  file/byte counts, enabled state)
- index cache: `$XDG_CACHE_HOME/amber/skill-index/` (regenerable)

The ledger records where a skill came from and hashes the installed `SKILL.md`,
so a later update can refuse to overwrite local edits.

## Updates and removal

On an installed, managed skill, `u` fetches the remote detail and compares its
`SKILL.md` blob hash with the ledger. If it differs, the row gets an update
marker and a second `u` applies the update, preserving the enabled state. If the
installed `SKILL.md` no longer matches the ledger hash, the update is refused
(`has local edits; update refused`).

`D` removes the tree and its ledger entry. Overrides (`disable`, `block`) are
deliberately left alone.

## Mirrors and offline registries

The explorer reads three environment overrides, useful for a self-hosted or
air-gapped mirror:

```sh
export AMBER_SKILLS_INDEX_URL=http://mirror.example
export AMBER_GITHUB_API_URL=http://mirror.example
export AMBER_GITHUB_RAW_URL=http://mirror.example
```

Empty values keep the public defaults.

## Current limits

This is the first release of the surface. Not implemented yet:

- markdown body preview / pager, file browser, audit verdicts and token cost
  (#398);
- the review/audit install gates, per-file progress and multi-select (#399);
- the full `/skills` verb tree and `amber-cli skills … --json` (#400);
- the diff pager and update-all (#402);
- shelves (For you / Trending / Top / Official / Collections), facets and
  offline/429 states (#405–#407).

The explorer itself is exercised by the unit suite and an open/close PTY
scenario; the hermetic install/update/remove PTY path and the remaining patch
coverage are tracked in #446.

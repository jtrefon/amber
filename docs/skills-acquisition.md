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
installs, license, category, file count). Press `i` for the install sheet.

```text
/skills search tdd
/skills install acme/skills/tdd
/skills installed
```

## Keys

| Key | Action |
|---|---|
| `/` | focus the search field |
| `j`/`k`, arrows | move |
| `Enter` | leave the search field / load the selected detail |
| `i` | open the install sheet |
| `p` | preview the skill body (also satisfies the review gate) |
| `v` | audit readout: risk-signal counts, and "not audited" when there is no audit source |
| `6` | Installed shelf |
| `u` | check for an update; press again to apply |
| `D` | remove (confirmation required) |
| `A` | adopt an unmanaged skill directory (adds provenance, rewrites nothing) |
| `x` | enable / disable through the catalog override |
| `b` | block |
| `q`, `Esc` | close |

## Command forms

Every screen action has a command form; the explorer is a view of the same
backends.

| Command | What it does |
|---|---|
| `/skills` | open the explorer |
| `/skills browse` | the same, spelled out |
| `/skills search <query>` | open the explorer with the query already searched |
| `/skills list` | print the catalog (scope, origin, name, state) |
| `/skills show <name>` | print one catalog entry |
| `/skills install <id> [--global]` | fetch the skill and open the install sheet |
| `/skills update` | open the Installed shelf, where `u` checks and applies |
| `/skills remove <name>` | open the Installed shelf filtered to that skill |
| `/skills sources` | print the index/GitHub endpoints and the cache size |

`enable`, `disable` and `block` stay under `/set skills` — curation is not
acquisition.

## The install sheet

`i` fetches the skill if needed, then opens one sheet: identity, trust tier,
files/size/token cost, license, risk signals from the body scanner, scope
(`project` is the default), name collisions, and whether to enable it now.

Two gates are named in the status line rather than drawn as a dead button:

- **Review.** A community or unverified source cannot be installed until `p`
  has shown the body, or `x` explicitly opts into an unread install. Official
  and audited sources bypass it.
- **Collision.** If the selected scope already holds a directory of that name,
  Enter replaces it and `r` refuses; Tab switches to the other scope, which
  starts with its own collision state.

The anonymous skills.sh API exposes no audits, so the sheet says **not
audited** instead of implying a verdict. `v` in the explorer reports the
risk-signal counts with the same honesty.

## Where skills live

- project scope: `<project>/.amber/skills/<name>/` (shareable, default)
- global scope: `~/.config/amber/skills/<name>/`
- provenance: `<skills root>/.lock.json` (id, source, ref, blob hash, local hash,
  file/byte counts, enabled state)
- index cache: `$XDG_CACHE_HOME/amber/skill-index/` (regenerable)

The ledger records where a skill came from and hashes the installed `SKILL.md`,
so a later update can refuse to overwrite local edits. The Installed shelf is
the union of the catalog and both ledgers: a package installed a moment ago is
on the shelf without a restart, and its install path (which remove and update
need) comes from the ledger.

## Updates and removal

On an installed, managed skill, `u` fetches the remote detail and compares its
`SKILL.md` blob hash with the ledger. If it differs, the row gets an update
marker and a second `u` applies the update, preserving the enabled state. If the
installed `SKILL.md` no longer matches the ledger hash, the update is refused
(`has local edits; update refused`).

`D` removes the tree and its ledger entry. Overrides (`disable`, `block`) are
deliberately left alone. If the conversation has activated the skill, the
confirmation adds `active in this session until the conversation ends`.

## Headless use

`amber-cli` answers the acquisition surface before any model, plugin or config
loading:

```text
amber-cli skills search <query> [--limit N] [--json]
amber-cli skills list [--global] [--json]
amber-cli skills show <name|owner/repo/skill> [--json]
amber-cli skills install <id|owner/repo|url> [--global] [--yes] [--json]
amber-cli skills update [<name>|--all] [--check] [--json]
amber-cli skills remove <name> [--global] [--json]
amber-cli skills sources [--refresh] [--json]
```

An unattended install refuses a community source without `--yes` and names the
step that satisfies the gate:

```console
$ amber-cli skills install acme/skills/tdd
skills install: refusing to install 'acme/skills/tdd' unattended (community
source, no audit).
  read it with 'amber-cli skills show acme/skills/tdd', then pass --yes.
```

`update` compares the recorded remote hash with the ledger, reports
`up to date`, `update available`, `locally modified (update refused)` or
`updated`, and exits non-zero only when a step errored. `sources --refresh`
empties the regenerable cache.

## Mirrors and offline registries

The explorer and the CLI read three environment overrides, useful for a
self-hosted or air-gapped mirror:

```sh
export AMBER_SKILLS_INDEX_URL=http://mirror.example
export AMBER_GITHUB_API_URL=http://mirror.example
export AMBER_GITHUB_RAW_URL=http://mirror.example
```

Empty values keep the public defaults.

## Current limits

Not implemented yet:

- the update diff pager, update-all from the TUI and the explicit
  remote-deleted/renamed states (#402);
- per-file install progress, cancellation and multi-select installs (#399,
  #408);
- shelves (For you / Trending / Top / Official / Collections), facets and
  offline/429 states (#405–#407);
- the skill pack/well-known index and the optional authenticated v1 adapter
  (#409, #410);
- scanning `.agents/skills` as an interop root (#412).

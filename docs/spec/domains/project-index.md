# The project index

Status: design record, agreed 2026-10-15. Implementation phases:
`docs/project-index-tracker.md`. Library content spec:
`docs/spec/domains/coding-library.md`.

## The problem

An agent session rediscovers, every time, facts the project already knows: that
the work runs in docker compose, which container is shared with another project,
that the backend is .NET and the frontend Angular, where a deploy key lives. The
cost is not only tokens — it is the failure mode: the agent asks the user for
something the project could have told it, or acts on a wrong model of how the
project runs. `AGENTS.md` covers the conventions a project *declares*; nothing
covers the structural facts amber *derives and confirms*, and nothing keeps them
across sessions.

## Terms (fixed, so the documents do not wobble)

- **Project index** — the resident artefact: one small markdown file of project
  facts plus tags that select library entries. ~25 lines.
- **Library** — the dark content: coding knowledge, one concept per file,
  reached only through the working index. Can be large; costs nothing when idle.
- **Working index** — the per-turn, trigger-phrased lines generated from the
  project index's tags, injected as a head block (the skill-discovery slot).

## The three views of one truth

No queue, no bookkeeping. The file is the truth; the other two are caches of it.

| View | Lifetime | Why |
|---|---|---|
| **File** (`<state>/projects/<slug>/project-index.md`) | Durable | `index_add/update/remove` write it immediately — add/change/remove is always available, no session bookkeeping |
| **Tool result** | Mid-session | The call returns the canonical entry text, so the agent that just registered a brick knows what it registered without the prompt moving |
| **Prompt block** | Session-fixed | Rendered from the file **at activation**, cached, never re-rendered mid-turn. No live edit, so no KV-prefix invalidation |

Refresh points, and only these:

1. **Activation** (new session, or the plugin re-enabled) — renders from the file,
   so everything written last session is simply there.
2. **Compaction** — the context is rebuilt anyway, so the block is re-read then at
   no extra KV cost. Structural refresh only (re-read files, re-check hashes); the
   expensive synthesis happens once, at init.
3. **Explicit** (`/index refresh`) — admits the KV cost, for when a session is
   long and something changed that matters now.

A session that never compacts loses nothing: the next session renders from the
file. This is why the design does not depend on compaction happening.

## What the index holds

```markdown
# Project: turbo-shop
index v3 · refreshed 2026-10-15 · sources: docker-compose.yml@a1b2, api.sln@c3d4

## Shape
monorepo (turbo) · backend .NET 8 (services/api, services/worker) · frontend
Angular (apps/web) · Redis cache · Postgres 16 · runs in docker compose,
services share network `shop`

## How it runs
up:      docker compose up -d          (api :5080, web :4200)
tests:   dotnet test (api) · npm test (web)
logs:    docker compose logs -f api    ← the feedback loop for runtime work

## Where things are
artefacts: $ADO_PAT (dev container env — never printed) · $DEV_SSH (~/.ssh/config
           host `devbox`) · $GITHUB_KEY (~/.ssh/id_ed25519)
config:    api/appsettings.Development.json · web/src/environments/*.ts
shared:    container `shop-db` is shared with the billing project — recreate it
           and that project's data goes with it

## Library selection
#dotnet #efcore #unitofwork #dockercompose #postgres-migrations #angular #redis-cache
```

Four fact kinds: **shape** (what the project is), **how it runs** (including the
telemetry/log command, which is what makes runtime work possible at all),
**where things are** (artefacts *by mechanism*, never by value; shared
infrastructure with its consequence), and **selection** (the tags that pull
library entries into the working index).

## The toolset

Advertised as prose in the coding domain, so the agent judges when it is worth
it — there is no init step the user must run.

- **`index_init`** — scans (bounded, deterministic), proposes the index, asks for
  confirmation, writes it with provenance hashes. Offered by the agent when the
  value is obvious (compose present, monorepo, several stacks) or when the user
  asks. Approval-gated: structural, once.
- **`index_add`** — a brick, a service, an artefact, a shared resource. Carries
  name, trigger line, tags, reference (url or path). Free (no approval), redaction
  checked, append-logged.
- **`index_update`** / **`index_remove`** — same policy as add. A project matures
  mid-flight (Bootstrap → Tailwind, Redis added for throttling); friction here
  means the change does not get recorded.
- **`index_show`** — prints the current file. This is how the agent sees changes
  made during the session, given the prompt block is frozen.

## Entry format

One header, shared by project-index entries and library entries, so the working
index can be generated from either:

```
name: <short name>
trigger: <when this applies — the line the index shows>
tags: #a #b #c
reference: <url | path>            (optional)
```

The **first line of the body is the trigger** for library files: the index
renders it verbatim, so a concept cannot exist without declaring when it applies,
and a file cannot drift from its index line.

## Trust and safety

- **Derived material, not instruction.** The index is produced by scan + synthesis
  + user confirmation, so it is treated like any other material that arrived from
  outside the conversation: weighed, used, and reported when it conflicts. It
  never grants authority.
- **Precedence**: `AGENTS.md` (declared) > project index (confirmed) > detection
  (derived). The file records which claims are which.
- **Pointers, never values.** A secret is named by its mechanism (`$ADO_PAT` in
  the dev container env), and the coder prompt carries the rule: secrets are used
  through the environment that expands them, never read, never echoed; a value
  that surfaces in output is an incident to report. A redaction pass refuses
  anything on write that looks like a credential.
- **Private by default** (`<state>/projects/<slug>/`), promotable to `.amber/`
  when a team wants to share it. It names where credentials live.
- **Append log** of every mutation, and reread-before-write, so two windows do not
  silently lose each other's entries.

## Staleness

The file records the source files and their hashes. When a source moves, the block
says so — `derived from docker-compose.yml@a1b2, changed since` — and the next
refresh (compaction, activation, or explicit) repairs it. A wrong index is more
dangerous than none, so its age is visible rather than assumed current.

## Decisions

| # | Decision |
|---|---|
| PI-D1 | Coding-specific first, as the test ground. Extraction into a shared `context` plugin later is a namespace move, not a redesign |
| PI-D2 | No init step the user must run: the toolset is advertised, the agent offers `index_init` when the value is clear or the user asks |
| PI-D3 | Three views (file / tool result / prompt block), refresh only at activation, compaction, or explicit request |
| PI-D4 | The prompt block is frozen for the session — the render is cached at activation and never re-rendered mid-turn |
| PI-D5 | Mutations: `init` approval-gated; add/update/remove free, redaction-checked, append-logged |
| PI-D6 | Pointers, never values; private by default; promotable |
| PI-D7 | Library = bundled skills with trigger-phrased descriptions, reached through tags. Per-entry `enable\|disable\|block` comes free from the skills mechanism |
| PI-D8 | The scanner stays shallow (presence + JSON manifests). No YAML parser: compose service detail is filled by the agent through `index_add` |
| PI-D9 | Resident budget: the coding domain's resident prose (identity, workflow, fundamentals, paradigm, project index) is pinned at 110 content lines — the measured value, ratcheted so it may only shrink — asserted by a test |

## Open items

1. **Compaction hook** — confirm a plugin can subscribe and re-render safely *at*
   that moment (`ContextEventSource` and the compression observer exist; the
   plugin-facing path needs checking). Blocks PI-3 only.
2. **Demotion measurement** — how "never pulled" is counted, so index entries can
   self-prune. Manual pruning in v1.
3. **Web references** — a doc URL in an entry is reachable only through `curl` via
   bash (approval-gated). A coding-domain fetch tool stays deferred.
4. **Frozen-render test** — pin that nothing re-renders blocks mid-session and
   reintroduces a prefix change by accident.

# Provider-aware model picker — proposal

**Status:** proposed, awaiting sign-off
**Date:** 2026-10-06
**Supersedes:** nothing. Follows #226 (drawer filter + selection), which is separate.

## Problem

`/set model` lists only the **active** provider's catalogue.
`list_model_info_cached(cfg)` (`lib/model_probe.cpp:231`) reads one cache file, keyed by
`FNV-1a(api_base + "\n" + flavor)` (`lib/model_probe.cpp:71`). Measured on the author's
machine:

```
https://api.kilo.ai/api/gateway  ->  models-e330b0c8eff2c2f5.json   (the only file present)
http://192.168.18.86:8081/v1     ->  models-b5b94292b55a4d72.json   (never written)
https://openrouter.ai/api/v1     ->  models-6f9f69acd98a39d4.json   (never written)
```

Two consequences, both observed:

1. **No way to see which provider's catalogue is on screen.** Five provider *plugins* are
   enabled by default; two are *configured*. Nothing in the drawer distinguishes "enabled"
   from "configured", so browsing a list on `kilocode` while looking for an `openrouter`
   model is indistinguishable from the model being absent. The rows carry no owner at all.
2. **No way to find a model that only another provider has.** With six providers configured
   on the author's test machine, the active provider's catalogue is one sixth of the
   searchable space.

`stealth/` appearing to contain only Anthropic models is *not* a bug: `stealth` is a routing
alias on that gateway and its catalogue genuinely lists
`stealth/claude-opus-4.8`, `claude-sonnet-4.6`, `qwen3.6-plus`. The confusion was ours, not
the data's.

## Decisions (signed off by the author)

1. **Left/Right arrows switch provider tabs while the drawer is open.** The input line's
   cursor movement is suspended for the drawer's lifetime and released when it closes.
2. **Selecting a model switches the provider to that model's owner**, then sets the model.
   Both are selected together.
3. **Every provider's copy of a model is listed separately.** No de-duplication: if three
   providers offer `anthropic/claude-opus-4.8`, three rows appear, each labelled with its
   owner. Nothing is hidden behind a heuristic.
4. The provider is displayed next to the model but **visually distinct** — it must not blend
   into the model name.

## Design

### Keying

The command tree is `set.model.children[<leaf key>]`, and `child_row()` prints the leaf key
(`tui/drawer_rows.cpp:67`). Two providers offering one id would collide on the key, so the key
becomes composite: `<provider>::<model id>`.

`::` does not occur in provider names or in vendor-prefixed model ids, so the split is
unambiguous. The composite key is also *typeable*, so `/set model kilocode::anthropic/claude-opus-4.8`
works without the drawer.

### Aggregation

A new pure function owns the aggregation, so it is testable without a terminal or a network:

```
aggregate(configured providers, their parsed catalogues, active provider name)
  -> rows, in stable order, each (provider, model id, context)
```

Input is already available per provider: `ProviderService::available()`
(`include/agent/providers.h:91`) yields name + `api_base` + `flavor`, and the catalogue cache
is already keyed per `api_base`+`flavor`, so N providers means N cache files with **no cache
change**. Only providers with a configured `api_base` and a readable catalogue contribute.

### Selection

`ProviderService::select(name)` → `apply_selection(cfg, sel)`
(`include/agent/providers.h:97,118`) already performs the provider switch and loads that
provider's last-used model; the chosen model then overrides it, and the provider's config is
persisted via `ProviderService::save()`. Order matters and is fixed: **select provider, then
set model.**

Because `ModelInfo` carries no owner, the owner travels in the feed's action closure, which
already captures the model id:

```cpp
tui_.register_action(action, [this, provider, id](const std::string&) { ... });
```

No change to `ModelInfo`, no change to the tree key shape beyond the composite.

### Key handling

The TUI already owns this exact pattern — `Tui::scroll_mode_nav()`
(`tui/tui.cpp:777`) is consulted *before* the edit-key table, returns `false` when its mode is
off (so the key falls through to normal routing), and `true` when it consumed the key. The
provider tabs reuse it: guarded on `drawer_open_`, placed ahead of `kEditKeys`
(`tui/tui.cpp:807`). Release is therefore structural, not a remembered call site.

Left/Right keep their normal meaning whenever the drawer is closed, which is the whole of
"release the keys".

### Row rendering — the one non-obvious consequence

Drawer rows are **plain `std::string`s painted uniformly**: `child_row()` concatenates key and
help, and `draw_drawer_rows()` paints every row with `COLOR_PAIR(P_ASSISTANT)`. A provider
badge in a different colour is therefore not expressible today.

`drawer_rows()` will return a row that carries its parts rather than a finished string:

```
struct DrawerRow { std::string label; std::string detail; std::string badge; };
```

`label` (model) and `detail` (ctx) paint as now; `badge` (provider) paints in a distinct pair.
`drawer_entry_names()` keeps returning plain strings, because `CommandLine` dispatches Enter on
it and the renderer's selection index must address the same list.

### Tabs

Tabs are `All` plus one per provider that contributed rows. `All` groups rows by provider so a
provider's models occupy one contiguous block, which makes "jump to the anthropic tab" the same
mental operation as scrolling to the block.

While the user types, **each tab shows its match count for the current filter**. This is the part
that makes tabs worth having: it turns "where should I look?" from a guess into a decision.

## Delivery

Staged, because "one revert undoes one change" and this is a feature rather than a fix.

- **PR A — provider-aware list.** Composite keys, aggregation across configured providers,
  the provider badge, and provider-switching on selection. This is the substance: it makes
  "All" mean all, and it alone would have made the original confusion impossible.
- **PR B — tabs.** Tab strip, Left/Right hijack, per-tab selection, per-tab match counts.

PR B is presentation on top of PR A's data, so it lands as its own reviewable diff.

## Explicit non-goals

- No change to `/set model`'s meaning beyond "may also switch provider". A row for the active
  provider behaves exactly as today.
- No per-provider search/filter of the request path.
- No provider *creation*; `/provider` continues to own that.
- Unconfigured providers do not appear as tabs: they have no `/models` to fetch. Surfacing
  "enabled but unconfigured" is `/get provider list`'s job, and it already reports it.

## Risks

- **Terminal real estate.** A tab strip costs a row, and a badge costs width. On a 24-line
  terminal the drawer already shows ~21 rows. The strip replaces the static key-hint line;
  hints move to `?`.
- **Six parallel fetches.** Each provider's catalogue is ~500 KB against a 24 h TTL. Fetching
  them is asynchronous and must not block the UI thread. This is the same constraint
  `refresh_models_async` already honours.
- **Row count.** With every copy listed, "All" is the sum across providers — thousands of rows
  for six providers. Windowing already follows the selection (#226), so this is navigable, but
  it is why PR B exists.
- **`completions.json` documentation.** `commands.set.children.model.man` currently promises
  "selectable with Up/Down and Enter"; it must describe tabs and provider switching.

## Verification plan

Red first, per the repo's TDD rule. Pure seams get tests before implementation:

- aggregation order, provider labelling, skip-unconfigured, composite-key splitting
- composite key round-trip: split `provider::id` back to both parts
- tab derivation: `All` + one per contributing provider, empty-catalogue providers excluded
- per-tab match counts for a given filter, including zero
- selection per tab, and clamp on tab switch
- left/right routing: consumed while `drawer_open_`, released when closed

`tui/` has no harness coverage of its own (patch coverage reads 0% there today), so the
pty/e2e path is the only end-to-end signal; the pure seams carry the weight.
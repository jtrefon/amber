# Complexity burndown: to CCN 15 and LOC 25

Generated from `make complexity-report` and lizard on 2026-09-30. Regenerate
rather than trusting these numbers.

## Where we are

The project has **2489 functions / 35308 NLOC**, with **average CCN 3.8** and
**average length 12.1 lines**. The codebase is well-factored *on average* — the
debt is concentrated in a few outliers, which is what makes this tractable.

| cap (CCN <= 15) | over-limit | share of functions |
|---|---|---|
| LOC 50 (today) | **49** | 2.0% |
| LOC 40 | 128 | 5.1% |
| LOC 30 | 259 | 10.4% |
| **LOC 25 (target)** | **368** | 14.8% |
| LOC 20 | 486 | 19.5% |
| LOC 15 | 731 | 29.4% |
| LOC 10 | 1053 | 42.3% |

At the target (CCN 15, LOC 25) the split is **lib 143, tui 112, bench 70,
tools 33, src 6, plugins 4**.

## The gap worth naming

`AGENTS.md` documents the aspiration as *"A method/function should stay under 10
lines with minimal branching"* — which would flag **42% of every function in the
project**. That is an aspiration, not a standard, and at LOC 10 it is not
reachable for C++: RAII, error handling and templates all cost lines before any
branching happens.

The enforced cap is 40, the target is 25, and the documented number is 10. This
document treats **25 as the target** and records 10 as aspirational only, so
nobody plans against a number the code cannot meet.

## Staged plan

The gate is a ratchet: it fails when a count *grows*. So the cap can be lowered
one step at a time, with the baseline regenerated at each step, and the gate is
never "put up" ahead of the code. That is the CX1 mistake in reverse — CX1 was a
check switched on before the code could pass it, which is why it sat deferred.

1. ✅ **CCN first, at the current LOC cap.** Done: the CCN axis is at **0** with
   an empty baseline, so it is now a hard cliff. (121 → 0 over the campaign.)
2. ✅ **LOC 50 -> 40.** Done: cap is 40, with **81** functions baselined as the
   ratchet (the table above predicted 128; the CCN work and the refactors
   removed the difference).
3. **LOC 40 -> 30** (~230 at the measured rate).
4. **LOC 30 -> 25** (~354).

At each stage the baseline shrinks and the cap tightens, so the gate is always
green *and* always stricter than before. There is never a red build caused by
the cap alone.

**Known weakness of the ratchet.** `tests/complexity_baseline.json` records a
count per *file*, not per function. A file can therefore swap one over-limit
function for another of the same count and still pass. The class-size gate does
not have this flaw (it records each type's line count). If the cap is tightened
again, record `(file, function, length)` first — otherwise each step licenses
churn within a file.

## Worklist — worst 15 at CCN 15 / LOC 25

| function | len | CCN |
|---|---|---|
| `bench::load_scenario` | 95 | **61** |
| `tui::form_focus::key` | 82 | **31** |
| `tui::md::flush_table` | 98 | 30 |
| `tui::RenderEngine::draw_status_bar` | 129 | 27 |
| `tui::text::wrap` | 90 | 27 |
| `tui::rich::wrap` | 85 | 26 |
| `tui::ansi_sgr::apply` | 76 | 25 |
| `tui::md::is_separator_line` | 46 | 24 |
| `tui::md::leave_block` | 87 | 23 |
| `tui::list_state::key` | 70 | 23 |
| `bench::compute_kpi` | 81 | 22 |
| `tui::SessionBrowserCore::key` | 66 | 22 |
| `tui::RenderEngine::build_view_without_working` | 61 | 21 |
| `lib::core_status_capabilities` | 90 | 21 |
| `tui::md::enter_block` | 70 | 20 |

Full list: `make complexity-report` — it sorts across the whole tree and names
the axis each one fails (dense / long / both).

## Notes carried forward

- **`bench::load_scenario` at CCN 61** is the worst function left anywhere —
  worse than `parse_compression_response` (43) and `Tui::run` (109 before its
  decomposition). It is in `bench/`, not `tui/`, and area-by-area work would
  have missed it.
- **`tui::text::wrap` (90/27) and `tui::rich::wrap` (85/26)** are suspiciously
  similar. Diff them before decomposing either: two near-identical wrapping
  implementations is the signature of the duplication found repeatedly in this
  campaign, and the cross-file detector will not flag them unless the blocks are
  textually identical.
- **`tools/read_tool.cpp` (103/19) and `tools/bash_tool.cpp` (83/19)** are the
  approval-gated security boundary. They deserve the characterization-test
  treatment `dispatch_tool_calls` and `shell_classify` got, not a quick split.
- **Pure `tui/` modules before paint code.** `form_focus`, `list_state`,
  `ansi_sgr`, `md::*`, `text::wrap` are ncurses-free and unit-testable. The
  drawing functions (`draw_status_bar`, `draw_drawer`, `Canvas::render`,
  `panel_view`, `info_dialog`) are only covered by the pty suite, so splitting
  them buys the gate a number and the codebase very little.

# Complexity enforcement and core decomposition

Status: **proposal, for sign-off** · 2026-09-24
Supersedes nothing; complements `docs/fix-proposal/tui-god-code-decomposition-2026-09-23.md`
and `docs/spec/tui/architecture.md`.

## 1. Why this exists

The TUI decomposition work exposed a bigger problem: the project documents a
size standard (methods under 50 lines, minimal branching) and CI appears to
check it, but the check **could never fail**. Measured with the project's own
thresholds, **121 functions violate them**.

### 1.1 The pipeline leak (four independent faults)

| # | fault | evidence |
|---|---|---|
| 1 | **Wrong count.** `make complexity` ran lizard with `--CCN 15 -L 50`, then counted with `grep -c WARN` — a token lizard never prints (its header is `!!!! Warnings (...) !!!!`). The count was always 0. | `Makefile.in` (old target) |
| 2 | **Non-failing by design.** The target printed the count and then `echo "clean (informational)"`, always exiting 0. CI's `complexity` job passed unconditionally. | same |
| 3 | **clang-tidy switched off for this.** `.clang-tidy` disables `readability-function-cognitive-complexity`, and `readability-function-size` has no thresholds configured — with defaults a 74-line function passes (verified). | `.clang-tidy:28` |
| 4 | **Fail-open.** With lizard absent the count was 0 and the target reported "clean". Reproduced locally. | — |

`tests/build_hygiene.sh` P5 is *not* a size gate: it checks that the `AGENTS.md`
audit table's numbers match the tree (a documentation ratchet on known debt).

### 1.2 What is now enforced

`tools/complexity_gate.py` + `tests/complexity_baseline.json`, wired as
`make complexity` (gate) / `complexity-report` / `complexity-update`:

- counts over-limit functions per file (**CCN > 15 or length > 50**);
- **ratchet**: the baseline records accepted counts, so existing debt cannot
  grow and a new file must be clean;
- **fails closed** (exit 2) when lizard is unavailable.

Verified: regression → exit 1, clean → 0, no lizard → 2.

## 2. Measured state

**121 over-limit functions**, by area and by shape:

| area | count | | shape | count |
|---|---:|---|---|---:|
| `lib/` | **47** | | dense **and** long | **63** |
| `tui/` | 44 | | dense only (CCN>15, ≤50) | 19 |
| `bench/` | 18 | | long only (≤CCN 15) | 39 |
| `tools/` | 9 | | | |
| `src/` | 2 | | | |
| `plugins/` | 1 | | | |

The three shapes need **different** responses:

- **dense and long (63)** — genuine god functions; decompose.
- **dense only (19)** — a line-count rule never sees these; they are the
  hardest to test. `lib/shell_classify.cpp output_redirect_target` (20 lines,
  **CCN 16**), `lib/mcp_transport_http.cpp header_cb` (23, **16**),
  `lib/dialect_openai.cpp decode_payload` (29, **18**).
- **long only (39)** — mostly tables/serializers/constructors; low risk. A pure
  size rule would force pointless splitting (this already happened: the
  `register_config_*_actions` split produced a 94-line table at **CCN 5**).

Worst offenders:

```
len  CCN  function
485  109  tui::Tui::run
416   95  bench::run_one_scenario
373   82  src/main
200   61  agent::classify_shell
185   38  agent::dispatch_tool_calls
170   42  bench::render_scorecard
145   20  agent::CompressionPipeline::compress
129   72  agent::Config::load
129   27  tui::RenderEngine::draw_status_bar
```

**The core is where it hurts**: `lib/` carries 47, in the paths that decide
correctness. `Config::load` at **CCN 72** is dense *and* short — precisely the
shape that is hard to test and easy to break.

## 3. The standard

Four independent caps, all hard, all ratcheted:

| axis | metric | tool |
|---|---|---|
| size | length ≤ **50** | lizard / `readability-function-size.LineThreshold` |
| branching | CCN ≤ **15** | lizard `--CCN` |
| nesting | depth ≤ **4** | `readability-function-size.NestingThreshold` (verified) |
| readability | cognitive ≤ **25** | `readability-function-cognitive-complexity.Threshold` (verified) |

Why all four: a 20-case `switch` scores high CCN but reads fine; deep nesting
scores low CCN and reads badly; a serializer is long but branch-free. Size and
branching are orthogonal — of 121 violations, **63 are both, 19 branching-only,
39 size-only**, so any single metric misses 19 or 39.

**Legitimately long functions are exempted explicitly**, not by weakening the
rule: an entry in the `AGENTS.md` audit table stating *why* (table/serializer)
and carrying its CCN. Every exception becomes a visible, argued decision.

### 3.1 Still staged

The two clang-tidy checks (nesting, cognitive) are **not yet enabled**: enabling
them makes the `lint` job fail on *any touched file* with an over-limit
function, including currently-open work. Recommendation: enable them now and
fix findings as files are touched — the attrition ratchet — because the point is
to stop debt at the moment it is written.

## 4. Target architecture: the core

`lib/` is a flat namespace of 47 over-limit functions. The decomposition is
**by responsibility, not by line count**, and follows the layering already
established for the TUI (L1 domain / L2 application / L3 ports / L4 adapters /
L5 composition), minus the UI concerns:

| cluster | worst offenders | direction |
|---|---|---|
| **config** | `Config::load` 129/**72** | split by section (llm, policy, detection, compression) behind a validated reader; the CCN is in the key dispatch |
| **shell classification** | `classify_shell` 200/**61**, `output_redirect_target` 20/**16** | a rule table over pure predicates; the branching is the rule set, so it becomes data |
| **tool-call parsing** | `extract_tool_calls_from_text` 157/33 | separate "scan" from "validate/repair" |
| **dispatch** | `dispatch_tool_calls` 185/38 | one handler per tool-call outcome; the switch becomes a table |
| **compression** | `CompressionPipeline::compress` 145/20, `apply_classification` 160/42 | split classify / extract / rebuild (the pipeline is already pure per its spec) |
| **agent loop** | `Agent::run_compression` 116/14 | long-but-simple; exempt or split by phase |
| **workspace** | `Workspace::confine` 81/14 | pure path validation → L1 |
| **plugins** | `spawn_and_handshake` 80/13 | transport vs handshake |
| **dialect** | `append_messages` 69/14, `decode_payload` 29/**18** | request assembly vs SSE decode |

Guiding rules, consistent with the existing specs:

- **L1 first.** Anything that is a pure decision (rule tables, validation,
  parsing) moves to a pure, unit-tested unit. This is what pays back coverage.
- **Tables over switches.** A `switch` with N cases and low logic becomes a
  table — this *reduces CCN while keeping the code together*, which is the
  right answer for the 39 "long only" cases.
- **No behaviour change.** Every extraction is characterization-first: pin the
  behaviour in a test, move, keep it green.
- **Public headers stay public.** `lib/`'s headers are the CLI/TUI contract;
  new L1 units go behind them.

## 5. Phased plan

| phase | scope | exit criteria |
|---|---|---|
| **C0** (done) | complexity gate real, fail-closed, ratcheted | `make complexity` gates; self-tested |
| **C1** | enable clang-tidy nesting + cognitive caps | `lint` fails on a new >50-line/high-nesting function |
| **C2** | `lib/` clusters above, worst-first (`Config::load`, `classify_shell`, `dispatch_tool_calls`) | each split is pure-L1 + tests; baseline ratcheted down |
| **C3** | `tui/` remainder (already scoped as G1..G9 / FIX-043..051) | `Tui::run` ≤ 40 lines (P0.3–6 keystone) |
| **C4** | `bench/`, `tools/`, `src/` | entry points (`main` 373/82, 165/38) reduced to arg-parse + wiring |
| **C5** | the 19 branching-only cases | each ≤ CCN 15 |

Each phase lands independently, ratcheting `tests/complexity_baseline.json`
downward — so the number in the baseline is the progress metric, and it can only
improve.

## 6. Definition of done

- `make complexity` green with a baseline of **0** violations.
- No function > 50 lines, CCN > 15, nesting > 4, cognitive > 25, except entries
  in the exemption table (each with a stated reason).
- The exemption table contains only tables/serializers/constructors.
- Branch coverage rises (CCN ≈ the minimum number of paths to test; today it is
  **43.8%**).

## 7. Decisions needed

1. **Enable the clang-tidy nesting/cognitive caps now (C1) or after C2?**
2. **Exemption format** — extend the `AGENTS.md` audit table, or a dedicated
   `docs/complexity-exemptions.md`?
3. **Core first or TUI first?** `lib/` has more violations and more risk; the
   TUI keystone (`Tui::run`, CCN 109) is already designed.

# Agentic Architecture Review — why our tool economy is weak, and how to close the loop

Status: strategy proposal (no code changes yet)
Evidence base: 12 benchmark runs across 10 models, wire-level debugging, competitor
system prompts (opencode fetched verbatim; Claude Code / Codex from their public
documentation).

## 1. The baseline

Qwen3.6-27B dense is the king (913/1000, 24/25, agentic 71, plan% 69) — validated
by official numbers (SWE-bench Verified 77.2, Terminal-Bench 59.3). The harness
works: it ranks models in the same direction as the official benchmarks.

But the absolute numbers are sobering **for every model**:

- read efficiency 35–50% (models make ~2× the reads an optimal executor needs)
- 104–142 tool calls per 25-scenario run vs ~48 optimal (plan% 35–47)
- redundant identical calls: 5–32 per run
- p-03: every model prefers `bash echo/cat` over the `write` tool for small edits

These are harness+model properties. The competition (Claude Code, Codex,
opencode, Kilo) ships the same class of models with materially better tool
economy. What do they have that we don't?

## 2. Competition review (grounded)

### opencode (prompt fetched, 8.5 KB)

- **No prose tool documentation.** The system prompt contains zero tool docs —
  tools carry their own OpenAI schemas. Our prompt stack is 14 KB of prose tool
  docs + the same information again in schemas.
- **TodoWrite tool**: the model maintains a structured task list across the
  session. Planning state lives in the tool, not only in context.
- **Task tool**: spawns sub-agents for parallel exploration/delegation.
- Interface-tone rules (MUST/NEVER about CLI output) — separate from agent
  behavior, and the only "forcing" in the prompt.
- Bash/Glob/Grep/Read/Write/WebFetch as the core set — and Read is used
  surgically (grep first, read ranges), because the model was RL-trained on
  that discipline.

### Claude Code (documented publicly)

- **TodoWrite** — same externalized task tracking.
- **Task** — sub-agents with isolated context for parallel work.
- Tool results are **raw output** — no envelope, no args echo, no meta.
- "Explore first, then act"; Read with ranges; Bash session state; aggressive
  context compaction; git checkpoints for rollback.
- System prompt ≈40 KB but mostly capability/UX text; tool discipline is in the
  model's RL training, not the prose.

### Codex (documented publicly)

- Shell-centric: one loop, shell as the primary tool, plan-first for large
  changes, checkpoint/undo via git, tests as the verification loop.
- Tool results raw; minimal prose.

### The common denominator

| Mechanism | opencode | Claude Code | Codex | amber |
|---|---|---|---|---|
| Externalized task tracking (todo/plan tool) | ✅ TodoWrite | ✅ TodoWrite | plan-first | ❌ none |
| Sub-agent delegation | ✅ Task | ✅ Task | ❌ | ❌ none |
| Raw tool results (no envelope) | ✅ | ✅ | ✅ | ❌ envelope |
| Prose tool docs in prompt | ❌ (schemas only) | minimal | minimal | ✅ 14 KB stack |
| Surgical read discipline (grep→read ranges) | trained-in | trained-in | trained-in | prompt-only |
| Aggressive context compaction | ✅ | ✅ | ✅ | conservative |

## 3. Root-cause hypotheses, ranked by evidence

**H1 — No working memory tool (STRONG).** Every competitive harness gives the
model an external task list. Ours forces all state into context. The measured
signature matches: models re-read files to re-derive state (2× reads), repeat
identical calls (5–32/run), and re-verify what they already know. A todo/plan
tool externalizes state → fewer reads, fewer repeats. *Testable: redundant,
steps, plan% before/after.*

**H2 — Verbose result envelope inflates context (STRONG).** Each result costs
~117 bytes of header + a full args echo + `[end]`, on top of the output itself.
At 104–142 calls/run that's a significant fraction of the 32k pre-compression
budget — context grows faster → later turns degrade → models compensate by
re-reading (the redundancy feedback loop). Competitors return raw output.
*Testable: tokens/run, steps, redundant.*

**H3 — Duplicated tool documentation (MEDIUM).** Every tool is documented twice
(14 KB of prose + OpenAI schemas). Long prompts get skimmed → wrong arguments →
failed calls (5–18/run). *Testable: tool_failures.*

**H4 — No sub-agents (MEDIUM).** Parallel exploration/delegation is a core
competitive mechanism; requires repo-level scenarios to measure. *Gate: the
repo-level suite must exist first (H5 in BENCHMARK.md).*

**H5 — Model training fit (ENVIRONMENTAL).** The strongest models are RL-trained
on formats close to their production harnesses (raw results, todo tools,
shell-centric traces). Qwen3.6-27B wins our harness because it is the most
agentic-RL-trained model we run and tolerates our custom format best. Adopting
OpenAI-conventional result formats reduces the format burden on every model.
*Testable via the same before/after runs as H2/H3.*

**H6 — Conservative compaction (LOW-MEDIUM).** Context grows to ~32k before
compression; competitors compact earlier with richer retention instructions.
*Testable with compression-stress scenarios (hermetic infra exists).*

## 4. Strategy — close the loop, one change at a time

Per the one-change-at-a-time rule, each item is a separate TDD cycle with a
before/after benchmark run on the baseline models (Qwen3.6-27B dense local,
Laguna S 2.1 free cloud):

| # | Change | Hypothesis tested | Metric gate |
|---|---|---|---|
| P1 | **Todo/plan tool** — a `plan` tool the agent maintains (list/update/complete); exposed like any tool, no loop changes | H1 | redundant ↓, steps ↓, plan% ↑ |
| P2 | **Lean tool results** — drop args echo for write, shrink header, keep status only | H2 | tokens/run ↓, steps ↓, redundant ↓ |
| P3 | **Schema-first tools** — trim tools.md prose to the envelope contract; schemas carry descriptions | H3 | tool_failures ↓ |
| P4 | **Sub-agents** — Task tool with isolated context; architecture work, gated on repo-level scenarios | H4 | multi-file scenario scores |
| P5 | **Compaction tuning** — lower threshold, richer retention instruction | H6 | compression-stress scenarios |

Non-goals: interface MUST/NEVER tone (the empowerment philosophy stays — the
benchmark will tell us if it costs anything); security gates; approval flow.

## 5. What winning looks like

The executable plan (architecture + specs + TDD lists + benchmark gates for
every item) is in `docs/plan/agentic-fix-plan.md` — a self-contained brief for
the implementing session.

P1 alone should move redundant calls toward single digits and raise plan% toward
60+ on the baseline models. Each subsequent item compounds. The BENCHMARK.md
tracks every before/after — that is the proof loop.

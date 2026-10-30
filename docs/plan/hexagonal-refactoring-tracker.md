# Amber Architecture Refactoring Tracker & Execution Ledger

> **Status:** PREPARATION & AUDIT COMPLETE — READY FOR PHASE 1  
> **Target Architecture:** Hexagonal (Ports & Adapters) + CQRS + FSM + Pipeline Middleware  
> **Master Specification:** [`hexagonal-architecture-spec.md`](../architecture/hexagonal-architecture-spec.md)

---

## 1. Monolith Inventory & Burn-Down Baseline

This ledger tracks the decomposition of Amber's legacy god-classes and monoliths.

| Monolith File | Current Lines | Target Size | Primary Patterns to Extract | Status |
|---|---|---|---|---|
| `tui/tui_input.cpp` | **4,525** | $\le 400$ | Command Pattern (`SlashCommandRegistry`), Chain of Responsibility (`KeyEventRouter`), Controller | 🔴 Pending Phase 4 |
| `tests/agent_loop_test.cpp` | **4,241** | $\le 600$ | Break into targeted unit tests (`tests/application/`, `tests/domain/`) with mock ports | 🔴 Pending Phase 3 |
| `tests/plugin_runtime_test.cpp` | **3,625** | $\le 500$ | Split into isolated adapter and registry contract tests | 🔴 Pending Phase 4 |
| `tests/bench_test.cpp` | **3,208** | $\le 500$ | Split into scenario unit tests | 🔴 Pending Phase 4 |
| `tui/skills_explorer.cpp` | **1,592** | $\le 300$ | Separate MVC/View renderers from Skill query controller | 🔴 Pending Phase 4 |
| `lib/agent.cpp` | **1,526** | $\le 250$ | `TurnFsmCoordinator`, `PromptAssemblyBuilder`, `LoopDetectionGuard`, `CompressionCoordinator` | 🔴 Pending Phase 2 |
| `tui/tui.cpp` | **1,387** | $\le 350$ | TUI Facade delegating to WindowManager, EventRouter, and SessionController | 🔴 Pending Phase 4 |
| `tui/skills_browse_core.cpp`| **1,119** | $\le 250$ | Decompose browse state from widget rendering | 🔴 Pending Phase 4 |
| `include/agent/agent.h` | **576** (184 code) | $\le 80$ | Extract enums, `UsageSample`, `AgentHooks`, `PromptBlock` to dedicated headers | 🔴 Pending Phase 1 |
| `tui/tui.h` | **435** (200 code) | $\le 100$ | Extract inline queue types and decouple nested manager declarations | 🔴 Pending Phase 4 |
| `tui/slash_dispatcher.h` | **380** (200 code) | $\le 60$ | Replace 200-line method declarations with dynamic command registration | 🔴 Pending Phase 4 |

---

## 2. Refactoring Phases & Work Breakdown Structure (WBS)

```
[Phase 0: Tooling & Static Analysis]
                 │
                 ▼
[Phase 1: Domain Leaf Types & Events]  ◄── Zero Risk, 100% Backward Compatible
                 │
                 ▼
[Phase 2: Inbound/Outbound Ports]      ◄── Establish Pure Contracts
                 │
                 ▼
[Phase 3: Agent Application Layer]     ◄── FSM, Pipeline, and CQRS Extraction
                 │
                 ▼
[Phase 4: TUI Command & View Layer]    ◄── Slashes tui_input.cpp (4,525 lines)
                 │
                 ▼
[Phase 5: Gate Hardening & Cleanup]    ◄── Activate CI Architecture Gate (Cliff)
```

---

### Phase 0: Enforcement Tooling & Gate Foundation
*Objective: Build the safety rails before touching any production C++ code.*

- [ ] **Task 0.1**: Create `include/agent/ports/port_traits.h` containing C++17 `static_assert` type traits (`is_valid_port_v`, `is_valid_event_v`, `is_valid_projection_v`).
- [ ] **Task 0.2**: Create `tools/architecture_gate.py` with:
  - Dependency DAG include scanner (forbidden layer back-edges).
  - Filename pattern suffix grammar validator.
  - One-type-per-file parser using `cpp_source.py`.
- [ ] **Task 0.3**: Create `tests/gates/test_architecture_gate.py` with unit tests for the gate (clean vs. mutated violation fixtures).
- [ ] **Task 0.4**: Wire `architecture-check` into `GNUmakefile` and `Makefile.in` under `make check` (set initially to ratchet mode via `tests/architecture_baseline.json`).

---

### Phase 1: Domain Leaf Types & Events Extraction
*Objective: Eliminate type bundling in `include/agent/agent.h` with zero behavioral risk.*

- [ ] **Task 1.1**: Extract `enum class RunState` into `include/agent/domain/types/run_state_type.h`.
- [ ] **Task 1.2**: Extract `enum class Approval` into `include/agent/domain/types/approval_type.h`.
- [ ] **Task 1.3**: Extract `struct UsageSample` & `enum class UsageSource` into `include/agent/domain/types/usage_sample_value.h`.
- [ ] **Task 1.4**: Extract and standardize immutable domain events into `include/agent/domain/events/`:
  - `turn_started_event.h`
  - `turn_ended_event.h`
  - `tool_requested_event.h`
  - `tool_completed_event.h`
  - `token_streamed_event.h`
  - `compression_completed_event.h`
- [ ] **Task 1.5**: Re-export all extracted types in `include/agent/agent.h` using forwarding `#include` directives to ensure all existing callers compile unchanged.
- [ ] **Task 1.6**: Run `make test`, `make lint`, `make check` to verify zero regression.

---

### Phase 2: Inbound & Outbound Port Contracts
*Objective: Decouple interfaces from concrete implementations.*

- [ ] **Task 2.1**: Define Outbound Ports in `include/agent/ports/outbound/`:
  - `llm_port.h` (replaces direct `LLMClient` coupling)
  - `tool_port.h` (replaces direct `Tool` coupling)
  - `storage_port.h` (replaces direct `MemoryStore` coupling)
  - `event_bus_port.h` (replaces `EventBus*` raw pointers)
- [ ] **Task 2.2**: Define Inbound CQRS Ports in `include/agent/ports/inbound/`:
  - `commands/execute_turn_port.h`
  - `commands/cancel_turn_port.h`
  - `queries/agent_projection_port.h`
- [ ] **Task 2.3**: Unify `AgentHooks` into `include/agent/ports/inbound/agent_observer_port.h` (driving adapter subscriber).
- [ ] **Task 2.4**: Retrofit existing adapters (`dialect_openai.cpp`, `bash_tool.cpp`, `memory_store.cpp`) to implement their respective `_port.h`.

---

### Phase 3: Application Layer Deconstruction (`lib/agent.cpp`)
*Objective: Dismantle the 1,526-line `Agent` monolith into clean, single-responsibility components.*

- [ ] **Task 3.1: State Machine (FSM)**:
  - Create `lib/application/fsm/turn_fsm_coordinator.{h,cpp}`.
  - Implement concrete state classes: `idle_state.cpp`, `prompt_assembling_state.cpp`, `model_streaming_state.cpp`, `tool_executing_state.cpp`, `turn_finalizing_state.cpp`.
  - Migrate the imperative while-loop in `Agent::run()` into deterministic FSM transitions.
- [ ] **Task 3.2: Prompt Assembly Builder**:
  - Extract `inject_prompt_blocks`, `collect_prompt_blocks`, and priority ordering into `lib/application/coordinators/prompt_assembly_builder.{h,cpp}`.
- [ ] **Task 3.3: Loop & Repetition Guards**:
  - Extract loop detection (`dispatch_with_loop_detection`, `stop_on_dispatch_loop`, `detect_text_loop`) into `lib/application/coordinators/loop_detection_guard.{h,cpp}`.
- [ ] **Task 3.4: Context Compression Coordinator**:
  - Extract compression pipeline orchestration into `lib/application/coordinators/compression_coordinator.{h,cpp}`.
- [ ] **Task 3.5: CQRS Read Projection**:
  - Implement `lib/application/queries/agent_projection_builder.{h,cpp}` to produce immutable snapshot projections for the UI.
- [ ] **Task 3.6: Thin Facade**:
  - Reduce `Agent` (`lib/agent.cpp` $\rightarrow$ `lib/application/coordinators/agent_facade.cpp`) to a thin coordinator ($\le 250$ lines).
- [ ] **Task 3.7**: Split `tests/agent_loop_test.cpp` (4,241 lines) into fast, isolated tests for the FSM, Prompt Builder, and Loop Guard.

---

### Phase 4: TUI Command & View Decomposition (`tui/tui_input.cpp`)
*Objective: Settle the 4,525-line monstrosity into modular command classes.*

- [ ] **Task 4.1: Slash Command Pattern**:
  - Define `tui/commands/slash_command_port.h`.
  - Create `tui/commands/slash_command_registry.{h,cpp}`.
  - Extract individual commands into dedicated classes:
    - `set_model_command.{h,cpp}`
    - `toggle_policy_command.{h,cpp}`
    - `provider_switch_command.{h,cpp}`
    - `help_command.{h,cpp}`
    - `mode_command.{h,cpp}`
- [ ] **Task 4.2: Key Routing (Chain of Responsibility)**:
  - Extract key routing out of `tui_input.cpp` into `tui/controllers/key_event_router.{h,cpp}`.
- [ ] **Task 4.3**: Verify `tui/tui_input.cpp` shrinks below 400 lines.
- [ ] **Task 4.4**: Write unit tests for individual slash commands in `tests/tui/commands/`.

---

### Phase 5: CI Hardening & Documentation Lockdown
*Objective: Seal the architecture and turn ratchet baselines into permanent cliffs.*

- [ ] **Task 5.1**: Burn down `tests/architecture_baseline.json` to zero.
- [ ] **Task 5.2**: Flip `tools/architecture_gate.py` to **fail-closed cliff mode** (no exceptions).
- [ ] **Task 5.3**: Add Clang-Tidy AST checks to `.clang-tidy` to prevent OS/cURL includes outside `lib/adapters/`.
- [ ] **Task 5.4**: Update [AGENTS.md](file:///Users/jack/Projects/amber/AGENTS.md) with the new architectural standards and guidelines.
- [ ] **Task 5.5**: Run full test matrix: `make clean && make -j && make test && make check && make lint && make analyze`.

---

## 3. Progress & PR Execution Log

| Date | Phase / Task | PR # | Commits | Affected Files | Verdict |
|---|---|---|---|---|---|
| *Pending* | Phase 0: Tooling & Gate | — | — | `tools/architecture_gate.py`, `tests/gates/` | ⏳ Ready |
| *Pending* | Phase 1: Domain Leaf Types | — | — | `include/agent/domain/`, `agent.h` | ⏳ Ready |
| *Pending* | Phase 2: Port Contracts | — | — | `include/agent/ports/` | ⏳ Queued |
| *Pending* | Phase 3: Application FSM | — | — | `lib/application/`, `lib/agent.cpp` | ⏳ Queued |
| *Pending* | Phase 4: TUI Commands | — | — | `tui/commands/`, `tui/tui_input.cpp` | ⏳ Queued |
| *Pending* | Phase 5: Gate Lockdown | — | — | `AGENTS.md`, `Makefile.in` | ⏳ Queued |

---

## 4. Architectural Invariants Sign-Off Checklist

Before any PR in this refactoring is armed for the merge train, it must satisfy all 6 invariants:

- [ ] **Inv 1: Domain Purity** — `include/agent/domain/` has zero includes of ports, adapters, application, or external I/O libs.
- [ ] **Inv 2: Port Abstractness** — Every `*_port.h` is 100% abstract (`= 0`) with a virtual default destructor.
- [ ] **Inv 3: No Direct Adapter Coupling** — Application coordinators only receive dependencies via abstract `_port.h` interfaces.
- [ ] **Inv 4: Naming Conformance** — Every added or renamed file strictly ends in one of the 13 approved `_<pattern_suffix>` tokens.
- [ ] **Inv 5: Single Responsibility** — No header defines more than 1 top-level class/struct.
- [ ] **Inv 6: Green CI Gates** — `make test`, `make lint`, `make hygiene`, `make complexity`, and `make architecture-check` report zero findings.

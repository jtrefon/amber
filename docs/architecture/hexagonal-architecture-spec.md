# Amber Hexagonal Architecture Specification & Structural Enforcement Masterplan

## 1. Architectural Vision

The goal of this refactoring is to transition Amber from an "Accidental Hexagon" (clean boundaries trapped in flat, bloated files) into an **Enforced Architectural Topology**.

Every file’s location in the directory tree and its suffix in the filename will unambiguously convey:
1. **Architectural Layer** (Hexagonal Boundary: Domain, Inbound Port, Outbound Port, Application, Adapter).
2. **Design Pattern Role** (Command, Query, Projection, State, Filter, Guard, Adapter, Facade, Builder).
3. **Allowed Dependency Invocations** (Strictly verified by automated CI gates).

---

## 2. Directory Taxonomy & Layer Boundaries

```
include/agent/
├── domain/                                # LAYER 0: PURE DOMAIN (No I/O, No External Libs)
│   ├── model/                             # Core Domain Aggregates & Entities
│   │   ├── context.h                      # Conversation context stack
│   │   ├── turn.h                         # Turn lifecycle and bounds
│   │   └── message.h                      # Message envelopes & roles
│   ├── events/                            # Immutable Domain Events
│   │   ├── turn_started_event.h
│   │   ├── turn_ended_event.h
│   │   ├── tool_requested_event.h
│   │   ├── tool_completed_event.h
│   │   └── token_streamed_event.h
│   └── types/                             # Pure Value Objects & Enums
│       ├── run_state_type.h               # Idle, Waiting, Thinking, Streaming enum
│       ├── approval_type.h                # AllowOnce, AlwaysAllow, Deny enum
│       └── usage_sample_value.h           # Immutable token usage metrics
│
├── ports/                                 # LAYER 1: PURE INTERFACES (Contracts)
│   ├── inbound/                           # Driving Ports (Invoked by UI / CLI / Bench)
│   │   ├── commands/                      # CQRS Write Side (Actions)
│   │   │   ├── execute_turn_port.h
│   │   │   └── cancel_turn_port.h
│   │   └── queries/                       # CQRS Read Side (Projections)
│   │       └── agent_projection_port.h
│   │
│   └── outbound/                          # Driven Ports (Invoked by Application)
│       ├── llm_port.h                     # LLM Model round-trip interface
│       ├── tool_port.h                    # Tool execution interface
│       ├── storage_port.h                 # Experience/memory persistence
│       └── event_bus_port.h               # Event publish/subscribe interface
│
├── pipeline/                              # Middleware Contracts (Pipes & Filters)
│   ├── prompt_middleware_port.h           # Prompt transformation step interface
│   └── tool_middleware_port.h             # Tool execution interception interface
│
└── state/                                 # State Machine Contracts
    └── turn_state_port.h                  # Abstract State in the Turn FSM

lib/
├── application/                           # LAYER 2: APPLICATION & USE CASES
│   ├── commands/                          # CQRS Command Handlers (Write Model)
│   │   ├── execute_turn_handler.h/.cpp
│   │   └── cancel_turn_handler.h/.cpp
│   ├── queries/                           # CQRS Projections (Read Model)
│   │   └── agent_projection_builder.h/.cpp
│   ├── fsm/                               # Turn State Machine (State Pattern)
│   │   ├── idle_state.h/.cpp
│   │   ├── prompt_assembling_state.h/.cpp
│   │   ├── model_streaming_state.h/.cpp
│   │   ├── tool_executing_state.h/.cpp
│   │   └── turn_fsm_coordinator.h/.cpp
│   ├── pipeline/                          # Middleware Step Implementations
│   │   ├── at_reference_filter.h/.cpp     # Expands @file references
│   │   ├── security_scrub_filter.h/.cpp   # Sanitizes prompt inputs
│   │   └── approval_policy_guard.h/.cpp   # Verifies tool permissions
│   └── coordinators/                      # Subsystem Orchestrators
│       ├── agent_facade.h/.cpp            # Clean external facade
│       ├── prompt_assembly_builder.h/.cpp # Assembles system & memory blocks
│       ├── compression_coordinator.h/.cpp # Context compaction orchestrator
│       └── loop_detection_guard.h/.cpp    # Repetition & loop guard
│
└── adapters/                              # LAYER 3: CONCRETE ADAPTERS (I/O, OS, cURL)
    ├── llm/
    │   ├── openai_dialect_adapter.h/.cpp
    │   ├── anthropic_dialect_adapter.h/.cpp
    │   ├── gemini_dialect_adapter.h/.cpp
    │   └── curl_transport_adapter.h/.cpp
    ├── tools/
    │   ├── bash_system_adapter.h/.cpp
    │   ├── process_system_adapter.h/.cpp
    │   └── mcp_rpc_adapter.h/.cpp
    └── storage/
        ├── json_memory_store_adapter.h/.cpp
        └── session_brief_adapter.h/.cpp

tui/
├── commands/                              # TUI Slash Commands (Command Pattern)
│   ├── slash_command_port.h               # Interface for any /command
│   ├── slash_command_registry.h/.cpp      # Dynamic command catalog
│   ├── set_model_command.h/.cpp
│   ├── toggle_policy_command.h/.cpp
│   └── provider_switch_command.h/.cpp
├── controllers/                           # Driving Adapters routing UI inputs
└── views/                                 # CQRS Read-Side Renderers
```

---

## 3. Strict Filename Grammar & Pattern Suffixes

Every C++ file (`.h`, `.cpp`) in `include/agent/` and `lib/` **must** adhere to the naming format:
$$\mathbf{<\text{domain\_concept}>\_<\text{pattern\_suffix}>.\{h,cpp\}}$$

### Permitted Pattern Suffixes and Semantic Roles

| Suffix | Architectural Layer | Responsibility / Pattern | Verification Contract |
|---|---|---|---|
| `_port.h` | `include/agent/ports/` | **Abstract Interface** | Abstract class (`struct`/`class`). Pure virtual methods (`= 0`), virtual default destructor, **no method definitions**. |
| `_event.h` | `include/agent/domain/events/` | **Immutable Domain Event** | Struct containing pure data payloads. All members `const` or non-mutating. |
| `_value.h` / `_type.h` | `include/agent/domain/types/` | **Value Object / Enum** | Plain data carrier or `enum class`. Zero I/O, zero external dependencies. |
| `_command.h/.cpp` | `ports/inbound/` or `tui/commands/` | **Command (CQRS / GoF)** | Intent to perform an action with typed parameters. |
| `_handler.h/.cpp` | `lib/application/commands/` | **Command Handler** | Executes a command against domain entities and ports. |
| `_query.h/.cpp` | `ports/inbound/queries/` | **Query (CQRS)** | Read request for projected state. |
| `_projection.h` | `ports/inbound/queries/` | **Read Projection** | Immutable snapshot of agent state tailored for UI/rendering. |
| `_state.h/.cpp` | `lib/application/fsm/` | **State Pattern (FSM)** | Concrete state implementation in the turn lifecycle. |
| `_filter.h/.cpp` | `lib/application/pipeline/` | **Pipes & Filters** | Single transformation or validation step in a processing pipeline. |
| `_guard.h/.cpp` | `lib/application/coordinators/` | **Specification / Guard** | Policy/safety check returning boolean decisions (e.g. loop detector). |
| `_builder.h/.cpp` | `lib/application/coordinators/` | **Builder Pattern** | Constructs complex domain objects (e.g. prompt assembly). |
| `_adapter.h/.cpp` | `lib/adapters/` | **Driven Adapter** | Concrete realization of an outbound port communicating with I/O. |
| `_facade.h/.cpp` | `lib/application/coordinators/` | **Facade Pattern** | Simplified entry point coordinating internal application services. |

---

## 4. The Include Dependency Matrix (Layer Invariant Rules)

To prevent architectural erosion, compile dependencies are strictly one-way:

| Source Layer | Permitted Includes | Forbidden Includes |
|---|---|---|
| **`domain/`** | Standard Library (`<string>`, `<vector>`, `<memory>`) | **NEVER** include `ports/`, `application/`, `adapters/`, or `tui/`. |
| **`ports/`** | `domain/`, Standard Library | **NEVER** include `application/`, `adapters/`, or `tui/`. |
| **`application/`** | `domain/`, `ports/`, `pipeline/`, `state/`, Standard Library | **NEVER** include concrete `adapters/`! Application depends **only** on abstract ports. **NEVER** include `tui/`. |
| **`adapters/`** | `ports/`, `domain/`, Third-Party Libraries (`curl`, `nlohmann`) | **NEVER** include `tui/` or internal application coordinators. |
| **`tui/`** (Driving) | `ports/inbound/`, `domain/types/`, Standard Library | **NEVER** include private application internals or raw adapters directly. |

---

## 5. Automated CI Architecture Gate (`tools/architecture_gate.py`)

A new, fail-closed gate will run in `make check` alongside Amber's existing gates (`hygiene`, `complexity`, `debt`, `nesting`).

### Enforcement Checks Performed by the Gate:
1. **Forbidden Dependency Scanner**:
   Parses every `#include` directive across all translation units. If an inclusion violates the **Include Dependency Matrix**, the gate fails immediately and prints the exact offending line.
2. **Filename Suffix Validator**:
   Scans `include/agent/` and `lib/`. Any file that does not end in one of the 13 approved `_<pattern_suffix>` tokens fails the gate.
3. **One-Type-Per-Header Enforcement**:
   No header file may define more than one top-level class or struct. Nested helper structs are permitted only if scoped inside the parent class.
4. **Port Purity Check**:
   Any file ending in `_port.h` is lexed: if it contains a non-default method body (`{ ... }`), or lacks a virtual destructor, it fails closed.

---

## 6. Migration Roadmap (Zero-Downtime Phasing)

Because Amber operates under a strict CI merge train, the migration will be rolled out across 5 isolated, backward-compatible PRs:

1. **PR 1: Domain Leaf Extraction**:
   Extract all enums, value objects, and domain events into `include/agent/domain/`. Old headers `#include` new domain files to maintain 100% backward compatibility.
2. **PR 2: Port Extraction & Suffix Renaming**:
   Create `include/agent/ports/` with abstract `_port.h` contracts. Convert `AgentHooks` into unified event subscribers.
3. **PR 3: Application Pipeline & FSM**:
   Extract the loop into `lib/application/fsm/` and middleware filters into `lib/application/pipeline/`. Decompose `lib/agent.cpp` from 1,526 lines down to `agent_facade.cpp` (~250 lines).
4. **PR 4: TUI Command Registry**:
   Convert the slash-command parser in `tui/tui_input.cpp` to the Command Pattern (`tui/commands/`), shrinking `tui_input.cpp` by over 2,500 lines.
5. **PR 5: Architecture Gate Activation**:
   Add `tools/architecture_gate.py` to `Makefile.in` and `.github/workflows/ci.yml`. The architecture is now permanently sealed and mechanistically enforced.

# Agentic Architecture

## Core Execution Loop

The agent loop is the heart of amber. It is a **ReAct-style tool-use loop** — the LLM
alternates between reasoning and tool invocation until it produces a final answer.

```
User Prompt
      │
      ▼
┌──────────────────┐
│  LLM Call        │  chat_once() — stream or buffered
│  (tools schema)  │
└────────┬─────────┘
         │
         ▼
  Parse Response
         │
    ┌────┴────┐
    │         │
    ▼         ▼
 tool_calls  text reply
    │         │
    ▼         ▼
 dispatch    return
 parallel
    │
    ▼
 push results
 to history
    │
    └──→ loop (max N iterations)
```

The loop lives in `Agent::run()` (`lib/agent.cpp`). Each iteration:

1. **`chat_once()`** — calls the LLM with the conversation history + tool schemas.
   Supports both streaming (`chat_stream`) and buffered (`chat`) modes. Stream
   events are forwarded to the UI via `AgentHooks`.
2. **Parse** — extract `tool_calls` or plain text from the assistant reply.
3. **Dispatch** — if tool calls are present, `dispatch_tool_calls()` runs each
   tool through the approval gate and executes it. Results are pushed back into
   `history_` as `role: "tool"` messages.
4. **Loop** — go back to step 1 for up to `cfg_.max_tool_iterations` turns.
5. **Return** — the final text reply is returned to the caller.

### Thread Model

```
┌─────────────────────┐     event queue (mutex + cv)    ┌─────────────────────┐
│  UI Thread          │ ◄══════════════════════════════ │  Agent Thread       │
│                     │     Token, StateChange,         │                     │
│  drain_events()     │     ToolCall, ToolResult,       │  agent_worker()     │
│  draw()             │     Approval, Done...           │  Agent::run()       │
│  getch()            │                                 │                     │
│  send_async() ──────┼────────────────────────────────►│  hooks → push ev    │
└─────────────────────┘                                 └─────────────────────┘
```

- Agent runs on a dedicated `std::thread` (see `Tui::agent_worker()` in `tui/tui.cpp`).
- All hooks write to a thread-safe `std::queue<AgentEvent>` under `std::mutex`.
- The UI thread drains the queue at the top of its 20 fps event loop.
- Approval dialogs use `std::promise`/`std::future` to block the agent thread
  while the user decides.

---

## Modes

Modes control **tool availability**, **approval policy**, and **orchestration
depth**. Switchable at runtime via `/mode read|write|yolo`.

| Mode  | Tools Available          | Approval        | Self‑Review | Use Case |
|-------|--------------------------|-----------------|-------------|----------|
| read  | search, grep, read only  | none            | off         | Ask questions about code |
| write | all                      | on by default   | off         | Normal development |
| yolo  | all                      | auto‑approve    | off         | Trusted, repetitive tasks |

### `/mode read`

- Only observation tools (search, grep, read, glob) are registered.
- The LLM can see write tools but they return "not available in read mode".
- No approval prompts.
- Self-review is skipped.

### `/mode write`

- All tools registered.
- Approval-gated tools (bash) show a confirmation dialog unless granted
  session-wide approval.
- No automatic self-review is performed (evaluator-optimizer loop; see
  "Evaluator-Optimizer (self-review) — not implemented" below).

### `/mode yolo`

- All tools registered.
- All approval gates are bypassed — tools execute immediately.
- No self-review.
- Intended for headless / scripted use or trusted workflows.

---

## Orchestration Patterns

### 1. Simple Loop (default, current)

```
LLM → tool_call → execute → LLM → tool_call → ... → text reply
```

Sequential, single-threaded within the agent. Tools run one at a time.
Used in `write` mode for most interactions.

### 2. Parallel Tool Dispatch

```
LLM → [tool₁ ║ tool₂ ║ tool₃] → aggregate → LLM → ...
```

When the LLM responds with multiple tool calls (OpenAI API allows this), they
are dispatched to a thread pool and executed concurrently. Results are
collected and pushed into history in call order. This is especially effective
for independent operations like "grep for X, read file Y, search for Z."

**Implementation notes:**
- Each tool call runs on a `std::thread` from a small pool.
- The agent thread blocks on `std::future::get()` for each result (in order).
- A failed tool does not cancel siblings — all results are collected.

### 3. Evaluator-Optimizer (self-review) — not implemented

```text
               ┌──────────────────────────────────────┐
               │                                      │
 generate ──► evaluate ──► needs revision? ──yes──► feedback
    │                                                  │
    │                                                  │
    └────── no (accept) ◄──────────────────────────────┘
```

Proposed design: after a write tool produces output, a second LLM call reviews
the result for correctness, completeness, and safety before the changes are
finalised. The review prompt would be minimal — the evaluator LLM sees the
tool call, its output, and a short rubric.

**Trigger conditions (proposed):**
- A write tool (write, edit, bash) returned successfully.
- Mode is `write` (always) or `yolo` (never).
- Self-review is not skipped due to quota / iteration limit.

**Status:** not implemented — no evaluator code exists in the tree.

### 4. Chain (planned)

```
step₁ → gate → step₂ → gate → step₃ → ...
```

Explicitly defined multi-step workflows. Each step is an LLM call or tool
block, connected by programmatic gates (e.g. "if validation fails, retry
up to 3 times"). Useful for structured tasks like "plan → implement → test."

### 5. Orchestrator-Workers (planned)

```
         orchestrator LLM (plans and delegates)
         /         |            \
    worker₁    worker₂        worker₃
   (grep)     (read file)    (edit file)
         \         |            /
          synthesizer LLM (optional)
```

A central LLM decomposes the task and delegates sub-problems to worker
calls (parallel or sequential). Results are synthesised into a final answer.

---

## Tool Execution Model

```
ToolRegistry::find(name)  ──►  Tool*  ──►  requires_approval?
                                                │
                                           ┌────┴────┐
                                           │         │
                                        allow      deny
                                           │
                                           ▼
                                    tool->execute(args)
                                           │
                                           ▼
                                    ToolResult {ok, output, error}
                                           │
                                           ▼
                                    history_.push_back(tool_msg)
```

- Tools are registered once at startup (`register_default_tools`).
- Each tool declares `requires_approval()` — bash returns `true`, read/write
  return `false`.
- The approval gate checks the current mode:
  - `read`: deny all write tools.
  - `write`: prompt if `requires_approval()` and not session‑approved.
  - `yolo`: always allow.
- Results are formatted as `role: "tool"` messages and appended to history
  so the LLM can consume them on the next turn.

---

## Event System (Agent → UI)

```
Agent hook fires → build AgentEvent → lock(mutex) → queue.push() → unlock
                                                      │
                                              (worker thread)
                                                      │
                                              (main thread)
                                                      │
                                              drain_events() → batch pop
                                                      │
                                                      ▼
                                              process events:
                                                Token      → stream_buf
                                                Reasoning  → reason_buf
                                                StateChange→ state_
                                                ToolCall   → append_line
                                                ToolResult → append_line
                                                Stats      → stats_
                                                Approval   → dialog + promise
                                                Done       → flush + autosave
                                                Error      → error message
```

| Event        | Payload          | Consumer              |
|--------------|------------------|------------------------|
| Token        | text             | `win().stream_buf`     |
| Reasoning    | text             | `win().reason_buf`     |
| StateChange  | RunState         | `state_`               |
| ToolCall     | name, args       | status bar / history   |
| ToolResult   | name, result     | status bar / history   |
| Status       | text             | `append_line(P_STATUS)`|
| Stats        | Stats            | `stats_`, `ctx_used_`  |
| Assistant    | text             | `append_line(P_ASST)`  |
| Approval     | summary, promise | menu_select → promise  |
| Done         | —                | flush, autosave        |
| Error        | text             | `append_line(P_STATUS)`|

---

## Future Directions

- **Routing / model selection** — route simple queries to a small model,
  complex coding to the full reasoning model.
- **Human-in-the-loop checkpoints** — pause at configurable points for
  user review (before file writes, before bash execution in `write` mode).
- **Tool dependency graphs** — let tools declare prerequisites so the
  dispatcher can parallelize or order them intelligently.

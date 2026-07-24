# TUI Architecture — Event-Driven Refactor

- **Status:** Design proposal (pending sign-off)
- **Applies to:** `tui/` (ncurses client)
- **Depends on:** `libagent_core.a`, `libagent_tools.a`, `ncursesw`
- **Design patterns:** Event Bus, Observer, Facade, Command, Strategy, Mediator
- **Current tech debt:** `Tui` class — 20 public methods, ~10 responsibilities, 2,438 lines across 5 files. Getsock.

---

## 1. Problem

### 1.1 Current state

The `Tui` class (187-line definition, 2,438-line implementation spread across 5
files) violates SRP. It owns:

| Responsibility | Methods | Files |
|---|---|---|
| Event loop & thread orchestration | 6 | tui.cpp |
| Geometry / layout | 6 | tui.cpp |
| ncurses rendering | 12 | tui_render.cpp |
| Streaming pipeline | 3 | tui_render.cpp |
| Session persistence | 7 | tui_session.cpp |
| Window management | 6 | tui.cpp |
| Slash commands | 25 | tui_input.cpp |
| Job management | 4 | tui_input.cpp |
| Settings / config | 7 | tui_session.cpp |
| Low-level helpers | 6 | tui_render.cpp / tui.cpp |

### 1.2 Choke points

| Issue | Impact |
|---|---|
| Single mutex-guarded event queue | All agent → UI traffic serialised through one lock |
| `/compress` runs synchronously on the main thread | UI freezes for 5-30s during compression |
| Modal dialogs block `drain_events()` | Agent approval can't be delivered while a dialog is open |
| Agent thread blocks on `std::promise<Approval>` | Agent thread sits idle waiting for UI response |
| Session saves run on the event loop tick | Disk I/O can stall a frame |
| Everything lives in one class | Impossible to test, reason about, or parallelise |

### 1.3 Why event-driven?

The current architecture uses direct method calls and a single event queue.
This creates implicit coupling between all components — the renderer knows
about sessions, the input handler knows about jobs, the event loop knows
about everything.

An event-driven architecture replaces direct coupling with a publish-subscribe
bus. A component emits an event; zero or more consumers react. The emitter
does not know who receives the event, and the receiver does not know who
sent it. This is the **Observer** pattern at the architectural level.

Benefits:
- Components become independently testable
- Thread boundaries are natural (emit on thread A, consume on thread B)
- Adding a new feature means adding a new subscriber, not modifying an
  existing class
- The main loop stays thin — it only dispatches events and paints

---

## 2. Target Architecture

### 2.1 Layer diagram

```
┌──────────────────────────────────────────────────────────────────┐
│                     Main Thread (20 fps tick)                     │
│                                                                  │
│  App::tick()                                                      │
│    bus->dispatch()            ← drain all pending events         │
│    layout->recalc()           ← recompute geometry if changed    │
│    input->poll()              ← wgetch(0) with 50ms timeout     │
│    renderer->draw()           ← wnoutrefresh + doupdate          │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
         ▲                              │
         │  emit()                      │  emit()
         ▼                              ▼
┌─────────────────────┐   ┌──────────────────────┐   ┌──────────────────┐
│ AgentService         │   │ CompressWorker        │   │ SessionManager    │
│ (dedicated thread)   │   │ (std::async on-demand)│   │ (background I/O)  │
│                      │   │                      │   │                  │
│ Owns Agent + LLM     │   │ Calls compress_now() │   │ Writes session   │
│ Emits tokens, stats, │   │ Emits CompressionDone│   │ files, workspace │
│ tool results, errors │   │                      │   │ state            │
└─────────────────────┘   └──────────────────────┘   └──────────────────┘
```

### 2.2 Event flow example

```
[User presses Esc during agent processing]

InputHandler::poll()
  → wgetch() returns 27 (Esc)
  → bus->emit(CancelRequested{})

EventBus::dispatch()
  → AgentService::on_cancel() received
    → cfg_.cancel_token.request()
  → StatusBar::on_state() received
    → state_ = Idle → redraw

App::tick() continues
  → renderer->draw() paints updated status bar
```

```
[Agent produces a token]

AgentService::run()
  → hooks_.on_token("Hello")
    → bus->emit(TokenReceived{"Hello"})

EventBus::dispatch()
  → ChatView::on_token("Hello")
    → stream_buf_ += "Hello"
    → sets dirty flag

App::tick()
  → renderer->draw()
    → ChatView paints buffered stream text
```

### 2.3 Thread safety model

| Component | Thread | Safety mechanism |
|---|---|---|
| `App`, `Renderer`, `InputHandler`, `Layout`, `ChatView`, `StatusBar`, `CommandPalette`, `DialogManager` | Main | Single-threaded — no locks needed |
| `EventBus` | Cross-thread | Per-slot `std::mutex`, lock-free dispatch for pending queue |
| `AgentService` | Own thread | All state internal. Emits events on agent thread, consumed on main via lock-free queue in EventBus |
| `CompressWorker` | `std::async` | Runs `compress_now()` on a temporary thread; emits result event when done |
| `SessionManager` | Dedicated or `std::async` | I/O happens off the main thread. Emits completion event. |

**Key rule:** The main thread never blocks. Any operation that could take
longer than a single frame (< 50ms) must run in a service thread and
communicate the result via an event.

---

## 3. Component Specification

### 3.1 EventBus

```
Path:    tui/event_bus.h / .cpp
Lines:   ~80
Pattern: Observer / Mediator
```

A typed publish-subscribe bus. Events are tagged structs (not an enum with
a variant payload) so each event type carries only the fields it needs.

```cpp
// Events
struct TokenReceived       { std::string text; };
struct ReasoningReceived   { std::string text; };
struct StateChanged        { agent::RunState state; };
struct ToolCallDispatched  { std::string name; agent::json args; };
struct ToolResultReceived  { std::string name; agent::ToolResult result; };
struct StatusMessage       { std::string text; };
struct StatsUpdated        { agent::Stats stats; };
struct AssistantDone       { std::string text; };
struct ApprovalNeeded      { std::string tool; agent::json args;
                             std::string summary;
                             std::shared_ptr<std::promise<agent::Approval>> response; };
struct ErrorOccurred       { std::string message; };
struct UserInput           { std::string text; };
struct CancelRequested     {};
struct CompressionRequested {};
struct CompressionDone     { agent::CompressionResult result; };
struct SessionSaved        { std::string id; };
struct WindowResized       { int rows; int cols; };
struct QuitRequested       {};

// Bus
class EventBus {
public:
    // Subscribe with a callable: bus->on<TokenReceived>(handler)
    template<typename E, typename F>
    void on(F&& f);

    // Emit synchronously (same thread) or queue for dispatch (cross-thread)
    template<typename E>
    void emit(E&& event);

    // Dispatch all queued cross-thread events. Called once per frame.
    void dispatch();
};
```

The `emit()` method:
- If called from the main thread → invoke handlers immediately
- If called from another thread → push to a lock-free queue, handlers
  fire on the next `dispatch()` call on the main thread

This eliminates the need for a mutex on every event. Only the cross-thread
queue needs synchronisation.

### 3.2 App

```
Path:    tui/app.h / .cpp
Lines:   ~50
Pattern: Facade
```

The thin orchestrator. Owns the tick loop and all component references.
Does nothing but call `dispatch → recalc → poll → draw`.

```cpp
class App {
public:
    App(std::shared_ptr<EventBus> bus,
        std::unique_ptr<Renderer> renderer,
        std::unique_ptr<InputHandler> input,
        std::unique_ptr<Layout> layout);

    void run();  // tick loop until QuitRequested

private:
    void tick();

    std::shared_ptr<EventBus> bus_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<InputHandler> input_;
    std::unique_ptr<Layout> layout_;
    // App does NOT own AgentService, SessionManager, etc.
    // Those are started/stopped externally and communicate via EventBus.
};
```

### 3.3 Renderer

```
Path:    tui/renderer.h / .cpp
Lines:   ~200
Pattern: Strategy
```

Owns all ncurses drawing. Sub-views are separate classes that register
their draw logic with the renderer.

```cpp
class Renderer {
public:
    explicit Renderer(std::shared_ptr<EventBus> bus);

    // Called once per frame by App
    void draw();

    // Views register themselves
    void set_chat_view(ChatView* v);
    void set_status_bar(StatusBar* s);
    void set_input_line(InputLine* i);
    void set_command_palette(CommandPalette* p);

private:
    ChatView* chat_ = nullptr;
    StatusBar* status_ = nullptr;
    InputLine* input_ = nullptr;
    CommandPalette* palette_ = nullptr;

    // Stage all changes to WINDOW* buffers, then one doupdate()
    void stage_chat();
    void stage_status_bar();
    void stage_input();
    void stage_palette();
};
```

The renderer subscribes to `WindowResized` and recalculates layout when
the terminal dimensions change.

### 3.4 Layout

```
Path:    tui/layout.h / .cpp
Lines:   ~50
Pattern: Value Object
```

Pure geometry — no rendering, no state beyond dimensions.

```cpp
struct Layout {
    int rows = 0, cols = 0;
    int chat_top = 0, chat_height = 0;
    int input_top = 0, input_height = 1;
    int status_top = 0;
    int palette_height = 0;

    void recalc(int term_rows, int term_cols);
};
```

### 3.5 InputHandler

```
Path:    tui/input_handler.h / .cpp
Lines:   ~150
Pattern: Command
```

Reads keyboard input, dispatches to command handlers, emits `UserInput`
or `CancelRequested` events. Owns the completer state machine.

```cpp
class InputHandler {
public:
    explicit InputHandler(std::shared_ptr<EventBus> bus);

    // Poll keyboard (non-blocking, 50ms timeout). Called once per frame.
    void poll();

    // Current input buffer (for rendering)
    const std::string& input() const { return input_; }

private:
    std::shared_ptr<EventBus> bus_;
    std::string input_;             // current input buffer
    palette::Completer completer_;  // tab-completion state
    bool drawer_open_ = false;
    int drawer_sel_ = 0;

    void handle_char(int ch);
    void handle_enter();
    void handle_tab();
    void handle_escape();
    void handle_resize();
    // Slash commands are now separate: CommandDispatcher
};
```

The `InputHandler` handles raw keystrokes. It builds the input buffer and
emits `UserInput{text}` on Enter. It does NOT interpret slash commands —
that's the `CommandDispatcher`'s job.

### 3.6 CommandDispatcher

```
Path:    tui/command_dispatcher.h / .cpp
Lines:   ~150
Pattern: Command
```

Subscribes to `UserInput` events. If the input starts with `/`, it parses
the command and dispatches to the appropriate handler. Otherwise it emits
a `PromptSubmitted` event to the AgentService.

```cpp
class CommandDispatcher {
public:
    explicit CommandDispatcher(std::shared_ptr<EventBus> bus);

private:
    void on_user_input(const UserInput& e);

    struct Cmd {
        std::string name;
        std::vector<std::string> aliases;
        std::string usage;
        std::string description;
        std::function<void(const std::string& arg)> handler;
    };
    std::vector<Cmd> commands_;
    void build_commands();
    void handle_help(const std::string& arg);
    void handle_window(const std::string& arg);
    void handle_compress(const std::string& arg);
    void handle_job(const std::string& arg);
    void handle_set(const std::string& arg);
    // ...
};
```

### 3.7 ChatView

```
Path:    tui/chat_view.h / .cpp
Lines:   ~150
Pattern: Observer
```

Subscribes to `TokenReceived`, `ReasoningReceived`, `ToolCallDispatched`,
`ToolResultReceived`, `StatusMessage`, `AssistantDone`. Maintains the
scrollback buffer. Handles streaming text assembly and markdown rendering.

```cpp
class ChatView {
public:
    explicit ChatView(std::shared_ptr<EventBus> bus, const md::Style& style);

    // Accessors for the renderer
    const std::vector<rich::Line>& lines() const { return lines_; }
    int scroll() const { return scroll_; }
    void set_scroll(int s);

    // Called by Renderer
    int height() const;

private:
    std::vector<rich::Line> lines_;
    int scroll_ = 0;
    std::string stream_buf_;    // unflushed streaming text
    std::string reason_buf_;    // unflushed reasoning text

    void on_token(const TokenReceived& e);
    void on_reasoning(const ReasoningReceived& e);
    void on_tool_call(const ToolCallDispatched& e);
    void on_tool_result(const ToolResultReceived& e);
    void on_status(const StatusMessage& e);
    void on_done(const AssistantDone& e);
    void flush_stream();
};
```

### 3.8 StatusBar

```
Path:    tui/status_bar.h / .cpp
Lines:   ~80
Pattern: Observer
```

Subscribes to `StateChanged`, `StatsUpdated`, `StatusMessage`.
Renders the bottom status line with model name, mode, connection indicator,
context gauge, clock, and running tool name.

```cpp
class StatusBar {
public:
    explicit StatusBar(std::shared_ptr<EventBus> bus);

    // Called by Renderer
    std::vector<Renderer::Seg> segments() const;

private:
    agent::RunState state_ = agent::RunState::Idle;
    agent::Stats stats_;
    std::string status_text_;
    std::string running_tool_;
    std::chrono::steady_clock::time_point last_tick_;

    void on_state(const StateChanged& e);
    void on_stats(const StatsUpdated& e);
    void on_status(const StatusMessage& e);
};
```

### 3.9 DialogManager

```
Path:    tui/dialog_manager.h / .cpp
Lines:   ~100
Pattern: Strategy / Command
```

Manages modal dialogues: config, settings, session browser, confirmation
dialogs. Each dialog is a separate class implementing a `Dialog` interface.
The DialogManager blocks input to the ChatView while a dialog is open,
but still dispatches events for the AgentService.

```cpp
struct Dialog {
    virtual ~Dialog() = default;
    virtual void draw() = 0;
    virtual int handle_key(int ch) = 0;  // return 0 to close
};

class DialogManager {
public:
    explicit DialogManager(std::shared_ptr<EventBus> bus);

    void push(std::unique_ptr<Dialog> dlg);
    void pop();
    bool active() const { return !stack_.empty(); }

    // Called by Renderer and InputHandler
    Dialog* top() const;

private:
    std::vector<std::unique_ptr<Dialog>> stack_;
};
```

### 3.10 AgentService

```
Path:    tui/agent_service.h / .cpp
Lines:   ~100
Pattern: Facade
```

Owns the Agent and runs it in a dedicated thread. Emits events for every
agent action. Accepts input via `PromptSubmitted` events.

```cpp
class AgentService {
public:
    AgentService(agent::Config cfg, agent::ToolRegistry& reg,
                 std::shared_ptr<EventBus> bus);

    void start(const std::string& prompt);  // thread-safe
    void cancel();                           // thread-safe
    bool busy() const { return agent_busy_.load(); }

private:
    std::shared_ptr<EventBus> bus_;
    agent::Config cfg_;
    agent::ToolRegistry& reg_;
    std::thread thread_;
    std::atomic<bool> agent_busy_{false};

    // Built once, shared across runs
    std::unique_ptr<agent::CompressionStrategy> compressor_;
    std::unique_ptr<agent::CompressionGate> gate_;
    std::unique_ptr<agent::MemoryStore> mem_store_;

    void worker(const std::string& prompt);
    void on_prompt(const PromptSubmitted& e);
    void on_cancel(const CancelRequested& e);
    void on_compress(const CompressionRequested& e);
};
```

The `worker()` method:
1. Creates an `Agent` with hooks that translate to `bus->emit()` calls
2. Calls `agent.run(prompt)`
3. On completion, emits `AssistantDone{text}`
4. Loops back to wait for the next `PromptSubmitted` event

The agent thread is NOT recreated per prompt — it's a persistent thread
that waits on a condition variable between prompts.

### 3.11 SessionManager

```
Path:    tui/session_manager.h / .cpp
Lines:   ~80
Pattern: Command
```

Subscribes to `AssistantDone` and `CompressionDone` events to trigger
autosaves. Provides async load/save operations via events.

```cpp
class SessionManager {
public:
    SessionManager(std::shared_ptr<EventBus> bus, agent::SessionStore& store);

    void snapshot(Window& w);     // take a snapshot (called before save)
    void autosave();              // save if dirty
    void load(const std::string& id);
    void save_as(const std::string& title);

private:
    std::shared_ptr<EventBus> bus_;
    agent::SessionStore& store_;
    agent::Session pending_;
    bool dirty_ = false;

    void on_done(const AssistantDone& e);
    void on_compress_done(const CompressionDone& e);
};
```

### 3.12 CompressWorker

```
Path:    tui/compress_worker.h / .cpp
Lines:   ~50
Pattern: Command
```

Subscribes to `CompressionRequested`. Runs `compress_now()` asynchronously
via `std::async`. Emits `CompressionDone` when finished.

```cpp
class CompressWorker {
public:
    explicit CompressWorker(std::shared_ptr<EventBus> bus,
                            agent::Agent& agent);
private:
    std::shared_ptr<EventBus> bus_;
    agent::Agent& agent_;
    std::future<void> pending_;

    void on_compress_requested(const CompressionRequested& e);
};
```

---

## 4. Composition Root (main.cpp)

```cpp
int main() {
    // 1. Bootstrap config, registry, services
    agent::Config cfg = load_config();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::register_default_tools(reg, jobs, cfg.cancel_token);

    auto mem_cfg = agent::load_experience_config(cfg);
    auto mem_store = agent::make_memory_store(mem_cfg);

    // 2. Create event bus
    auto bus = std::make_shared<EventBus>();

    // 3. Create UI components (main thread)
    auto layout = std::make_unique<Layout>();
    auto renderer = std::make_unique<Renderer>(bus);
    auto input = std::make_unique<InputHandler>(bus);
    auto commands = std::make_unique<CommandDispatcher>(bus);
    auto chat_view = std::make_unique<ChatView>(bus, md::Style{});
    auto status_bar = std::make_unique<StatusBar>(bus);
    auto dialogs = std::make_unique<DialogManager>(bus);
    auto session_svc = std::make_unique<SessionManager>(bus, store);
    auto compress = std::make_unique<CompressWorker>(bus, agent);

    // 4. Wire views to renderer
    renderer->set_chat_view(chat_view.get());
    renderer->set_status_bar(status_bar.get());
    renderer->set_input_line(input.get());
    renderer->set_command_palette(commands.get());

    // 5. Create agent (starts its own thread)
    auto agent_svc = std::make_unique<AgentService>(
        std::move(cfg), reg, bus, std::move(mem_store));

    // 6. Run
    App app(bus, std::move(renderer), std::move(input), std::move(layout));
    app.run();
}
```

---

## 5. Threading Model

### 5.1 Main thread (App::tick)

```
┌─────────────────────────────────────────────────────────┐
│  App::tick()  (20 fps, ~50ms per frame)                 │
│                                                          │
│  1. bus→dispatch()         ← 0.001ms  (process events)  │
│  2. input→poll()            ← 0-50ms  (wgetch timeout)  │
│  3. layout→recalc()         ← 0.001ms  (if resized)     │
│  4. renderer→draw()         ← 1-5ms   (ncurses paint)   │
│                                                          │
│  Total: 1-55ms — well within 50ms target                 │
└─────────────────────────────────────────────────────────┘
```

### 5.2 Agent thread

```
┌─────────────────────────────────────────────────────────┐
│  AgentService::worker()                                  │
│                                                          │
│  while (true) {                                          │
│    wait for PromptSubmitted or CancelRequested           │
│    if (cancel) { reset; continue; }                     │
│    agent.run(prompt)                                     │
│      → emits TokenReceived, ToolCallDispatched, ...      │
│      → emits AssistantDone                               │
│  }                                                       │
└─────────────────────────────────────────────────────────┘
```

The agent thread does not block on approval requests. Instead of
`std::promise<Approval>::get()` (which blocks the thread), the approval
hook emits an `ApprovalNeeded` event and the agent thread continues
processing or yields. The approval response arrives as a separate event.

### 5.3 No-blocking rule

| Operation | Before | After |
|---|---|---|
| `/compress` | `compress_now()` on main thread, UI frozen | `bus->emit(CompressionRequested{})`, returns immediately |
| Approval dialog | Agent thread blocked on `std::promise::get()` | Agent emits `ApprovalNeeded`, continues or idles |
| Session save | `fout << json; fout.close()` on main thread | Dispatched to SessionManager background I/O |
| `/job read` | `jobs_.read_delta(id)` on main thread | Emits event, result comes back asynchronously |

---

## 6. Error Handling

- Components that subscribe to events must not throw. Catch exceptions
  and emit an `ErrorOccurred` event.
- The `EventBus::emit()` catches exceptions from handlers and forwards
  them as `ErrorOccurred` events.
- If a service thread crashes, the service emits `ErrorOccurred` with
  the exception message and terminates the thread. The main thread
  displays the error and offers to restart.

---

## 7. Migration Plan

The refactor must not break the existing TUI during development. The
plan is to build the new components alongside the old Tui and migrate
one responsibility at a time.

### Phase 1: EventBus + App skeleton
- Create `EventBus`, `App`, `Layout`, `Renderer`
- Keep Tui class as-is but have it delegate rendering to `Renderer`
- Wire `EventBus` into the existing event queue
- **Deliverable:** Tui still works, but now has an EventBus

### Phase 2: Extract AgentService
- Extract agent thread management from Tui into `AgentService`
- Tui sends prompts via `bus->emit(PromptSubmitted)`
- AgentService emits events instead of calling Tui hooks directly
- **Deliverable:** Agent runs independently, Tui doesn't manage thread

### Phase 3: Extract InputHandler + CommandDispatcher
- Extract keyboard handling from `tui_input.cpp` into `InputHandler`
- Extract `/` commands into `CommandDispatcher`
- **Deliverable:** Tui no longer processes input directly

### Phase 4: Extract ChatView + StatusBar + DialogManager
- Extract rendering components from `tui_render.cpp`
- **Deliverable:** Tui is now just App + renderer assembly

### Phase 5: Extract SessionManager + CompressWorker
- Remove last responsibilities from Tui
- **Deliverable:** Tui class deleted entirely. Main function composes App.

### Tests
- Each component has its own test file in `tests/tui/`
- EventBus tests: verify emit/dispatch round-trip, thread safety
- ChatView tests: verify token accumulation, streaming flush
- CommandDispatcher tests: verify slash command parsing
- No ncurses dependency — Renderer is the only component that touches

---

## 8. Design Patterns Summary

| Pattern | Where | Why |
|---|---|---|
| **Event Bus** | `EventBus` | Decouples all components. Typed events. Thread boundary handled transparently. |
| **Observer** | All components subscribe to events | Components react to state changes without knowing who produced them. |
| **Facade** | `App`, `AgentService` | Thin façade over complex subsystems. |
| **Command** | `CommandDispatcher`, each dialog | Requests encapsulated as objects. Slash commands, dialog handlers, compression requests. |
| **Strategy** | `Renderer` | Drawing strategy is swappable (ncurses, debug, etc.). |
| **Mediator** | `EventBus` (central) + `App` (tick) | Components don't communicate directly — the bus mediates. |
| **Composition Root** | `main()` | All dependencies wired at startup. No service locator, no global state. |
| **Promise/Future** | `ApprovalNeeded` event | Cross-thread approval without blocking the agent thread. |
| **RAII** | All components | ncurses init/cleanup, thread join, file handles. |

---

## 9. File Map

```
tui/
├── app.h / .cpp                  # Main loop, orchestration
├── event_bus.h / .cpp            # Typed publish-subscribe bus
├── layout.h / .cpp               # Pure geometry calculations
├── renderer.h / .cpp             # ncurses drawing (single thread)
├── input_handler.h / .cpp        # Keyboard input, completer
├── command_dispatcher.h / .cpp   # Slash command routing
├── chat_view.h / .cpp            # Scrollback, streaming
├── status_bar.h / .cpp           # Bottom status line
├── dialog_manager.h / .cpp       # Modal dialog stack
├── agent_service.h / .cpp        # Agent thread lifecycle
├── compress_worker.h / .cpp      # Async compression
├── session_manager.h / .cpp      # Async persistence
├── canvas.h / .cpp               # (keep as-is)
├── markdown.h / .cpp             # (keep as-is)
├── palette.h / .cpp              # (keep as-is)
├── rich.h / .cpp                 # (keep as-is)
├── widgets.h                     # (keep as-is)
├── textutil.h / .cpp             # (keep as-is)
└── window.h                      # (keep as-is)
```

**Removed:**
- `tui.h` (replaced by individual component headers)
- `tui.cpp` (replaced by `app.cpp` + component implementations)
- `tui_input.cpp` (replaced by `input_handler.cpp` + `command_dispatcher.cpp`)
- `tui_render.cpp` (replaced by `renderer.cpp` + `chat_view.cpp` + `status_bar.cpp`)
- `tui_session.cpp` (replaced by `session_manager.cpp` + `dialog_manager.cpp`)
- `tui_main.cpp` (replaced by logic in `main.cpp`)

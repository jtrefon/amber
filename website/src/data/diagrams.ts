// Diagram sources, one entry per figure.
//
// These are the single source of truth for every diagram on the site: pages
// reference a diagram by name through <Diagram name="...">, never by pasting
// markup. Add a diagram here, not inline in a page. Every entry must also be
// listed in `diagramCatalog` below - the gallery page fails the build if one
// is missing.
//
// Mermaid renders labels as HTML, so angle brackets in a label are swallowed
// as tags. Use {placeholder} notation, never <placeholder>. A semicolon in a
// label is a statement terminator - never use one inside a diagram source.

export const diagrams = {
  // ---------------------------------------------------------------- runtime

  // The microkernel layering: hosts on top of the core, plugins contributing
  // into the core through the ledger.
  runtimeOverview: `flowchart TB
    hosts["Hosts<br/>amber TUI · amber-cli · amber-bench"]

    subgraph kernel["libagent_core: microkernel"]
        runtime["PluginRuntime<br/>capability ledger"]
        registries["Contribution registries<br/>tools · providers · commands · status · panels"]
        agent["Agent loop + Context"]
        events["Typed EventBus"]
        transport["LLM transport<br/>openai protocol"]

        runtime -->|install / unwind| registries
        registries --> agent
        agent --> events
        agent --> transport
    end

    bundled["Bundled plugins: compiled in<br/>providers · core tools · status segments · panels"]
    external["External plugins<br/>separate process · JSON-RPC over stdio<br/>tools only"]
    endpoint["OpenAI-compatible endpoint"]

    bundled -->|declare Capability| runtime
    external -->|tool.call| registries
    transport --> endpoint
    hosts -->|AgentHooks| agent`,

  // Plugin state machine. Mirrors the lifecycle table in the plugin docs.
  pluginLifecycle: `stateDiagram-v2
    [*] --> Registered
    Registered --> Active: enable - initialize + install
    Active --> Deactivated: disable - shutdown + unwind ledger
    Deactivated --> Active: enable
    Registered --> Failed: initialize returned false
    Active --> Failed: install failed
    Failed --> Registered: fix and reload
    Deactivated --> [*]`,

  // The ledger contract: declare, install, record; then shutdown and unwind in
  // reverse install order.
  capabilityLedger: `sequenceDiagram
    autonumber
    participant H as Host
    participant RT as PluginRuntime
    participant P as Plugin
    participant R as Registry
    participant L as Ledger

    H->>RT: enable(id)
    RT->>P: initialize(ctx)
    P-->>RT: true
    RT->>P: capabilities()
    P-->>RT: Capability[]

    loop each capability, declaration order
        RT->>R: install(services)
        R-->>RT: Contribution handle
        RT->>L: record(owner, Contribution)
    end

    Note over H,L: Active - every contribution reachable

    H->>RT: disable(id)
    RT->>P: shutdown()
    RT->>L: unwind(owner), reverse install order
    L->>R: remove() per Contribution

    Note over H,L: Deactivated - nothing reachable`,

  // External plugin tier: discovery, spawn-on-first-call, the JSON-RPC
  // handshake, and the shutdown notification.
  externalPluginHandshake: `sequenceDiagram
    autonumber
    participant H as amber harness
    participant PM as PluginManager
    participant P as plugin process

    H->>PM: enable(id)
    PM->>PM: discover manifest.json
    Note over H,PM: enabled - tools registered, namespace merged

    H->>PM: tool call
    PM->>P: spawn executable
    PM->>P: initialize protocol_version, settings, workspace
    P-->>PM: protocol_version, ok

    alt compatible
        PM->>P: tool.call name, args
        P-->>PM: ok, output, meta
        PM-->>H: tool envelope
    else version mismatch or ok false
        Note over H,P: unregistered - incompatible
    end

    H->>PM: disable(id)
    PM->>P: shutdown
    P-->>PM: exit`,

  // How a command contribution reaches the slash engine.
  pluginCommandBinding: `flowchart TB
    declare["Plugin declares CommandSpec<br/>root · help · man · subtree · handlers"]
    install["Ledger installs into CommandRegistry"]
    merge["Host merges the subtree into the command tree<br/>executable leaf action = plugin.{id}.{path}"]
    drawer["Drawer + completions"]
    dispatch["Dispatch resolves the handler<br/>from the live registry"]
    inert["Leaf with no handler<br/>documented, not executable"]
    toggle["enable / disable rebuilds the tree, no restart"]

    declare --> install
    install --> merge
    merge --> drawer
    merge --> dispatch
    merge --> inert
    toggle -.-> merge`,

  // Which plugin tier to pick.
  pluginTierChoice: `flowchart TD
    start(["Which plugin tier?"]) --> isolation{"Needs arbitrary or<br/>other-language code,<br/>or crash containment?"}
    isolation -->|yes| external["External plugin<br/>separate process · JSON-RPC<br/>tools only"]
    isolation -->|no| contributes{"Contributes anything<br/>beyond a tool?"}
    contributes -->|yes| core["Core plugin<br/>C++17 · compiled in"]
    contributes -->|no| wire{"OpenAI-wire<br/>compatible endpoint?"}
    wire -->|yes| file["No code needed<br/>a provider file is enough"]
    wire -->|no| core`,

  // ---------------------------------------------------------------- commands

  // The command surface is a JSON tree, not C++ branches.
  commandTreeStructure: `flowchart TB
    json["completions.json<br/>the single source of truth"]
    schema["node = { help, man, action, children }<br/>the last leaf of a branch carries the action"]
    json --> schema
    schema --> index["SettingRegistry indexes every node<br/>help · man · children · choices · ranges"]
    index --> keyed["namespaces keyed by full display path<br/>get.model is not set.model"]
    keyed --> resolve["resolve_key<br/>exact → get.key → set.key"]
    resolve --> unknown["unresolved → empty<br/>the caller reports an unknown command"]`,

  // One keypress to one closure. Dispatch never names a command.
  commandTreeDispatch: `flowchart TD
    line["Enter: /set policy read"] --> strip["strip the leading slash, trim, tokenize"]
    strip --> walk["walk the commands tree, consuming every token<br/>that names a documented child"]
    walk --> deepest{"any node matched?"}
    deepest -->|no| unknown["unknown command (try /help)"]
    deepest -->|yes| node["node = deepest documented match<br/>remaining tokens joined as the argument"]
    node --> has{"node carries an action?"}
    has -->|no| man["print the node man page<br/>documented, not executable"]
    has -->|yes| reg{"action registered?"}
    reg -->|no| man
    reg -->|yes| dispatch["dispatch(action, arg)"]
    dispatch --> safe["exceptions caught<br/>command error: ... to the status line"]`,

  // Dynamic values arrive as feed leaves, never as C++ lists.
  commandFeeds: `flowchart TB
    tree["completions.json<br/>documented namespaces"]
    feeds["feed leaves generated at runtime<br/>model list · policy rules · providers · jobs<br/>plugins · MCP servers · plugin commands"]
    tree --> merge
    feeds --> merge["merge_completions_json<br/>deep merge: static fields preserved,<br/>children unioned, feeds never clobber docs"]
    merge --> actions["each leaf carries a generated action<br/>and the feed registers its closure"]
    actions --> surface["drawer · completions · dispatch<br/>one tree, three readers"]`,

  // ------------------------------------------------------------------ build

  // What links what, and where the process boundary is.
  buildLayering: `flowchart TB
    subgraph core["libagent_core.a: UI-free core"]
        engine["agent loop · context · dispatch<br/>plugin runtime · event bus · ledger"]
        dialects["LLM dialects + HTTP transport"]
        bundled["bundled plugins, compiled in<br/>providers · core tools · hello"]
    end

    subgraph toolslib["libagent_tools.a"]
        tools["read · write · search · bash<br/>process · task · todowrite · skills"]
    end

    toolslib -->|depends on core| core

    core --> cli["amber-cli<br/>headless"]
    core --> tui["amber<br/>ncurses TUI"]
    core --> bench["amber-bench<br/>KPI harness"]
    toolslib --> cli
    toolslib --> tui
    toolslib --> bench

    tui --> nclib["ncurses + md4c"]
    cli --> curl["libcurl"]
    tui --> curl
    bench --> curl

    ext["external plugins<br/>separate processes, JSON-RPC<br/>never linked into any client"] -.->|process boundary| core
    note["core and tools appear twice on every link line<br/>to resolve the circular reference between them"]`,

  // The four plugin test layers.
  testLayers: `flowchart TB
    unit["1 · Unit<br/>a capability against a fake PluginServices"] --> integ
    integ["2 · Hermetic integration<br/>PluginRuntime + fake LLM client<br/>no network, ever"] --> dialect
    dialect["3 · Dialect, for providers<br/>pure request / parse / stream fixtures"] --> reg
    reg["4 · Regression: make test<br/>contributions must not leak into other tests"]

    guard["any test that activates a plugin redirects<br/>XDG_CONFIG_HOME to a scratch directory"] -.-> integ`,

  // ----------------------------------------------------------------- engine

  // One turn of the ReAct loop.
  agentLoopCycle: `flowchart TD
    run["Agent::run(prompt)"] --> seed["ensure_system_prompt<br/>history always starts with a system message"]
    seed --> loop{"iteration under max_tool_iterations<br/>and not done?"}
    loop -->|no| stop["hard stop<br/>loop detected, or tools kept failing"]
    loop -->|yes| gate{"compression gate<br/>context utilisation over threshold?"}
    gate -->|compress| rebuild["compression pipeline<br/>context rebuilt: clear + push"]
    gate -->|keep| assemble
    rebuild --> assemble["assemble the prompt copy<br/>head + tail blocks, after the gate"]
    assemble --> llm["LLM call, streamed via SSE"]
    llm --> parse["parse tool calls"]
    parse -->|none| confirm["silent confirm turn<br/>are you finished?"]
    confirm -->|done| reply["return the final reply"]
    confirm -->|continue| loop
    parse -->|calls| dispatch["dispatch tool calls<br/>parallel execution, ordered results"]
    dispatch --> push["push tool-role messages"]
    push --> loop`,

  // Every tool call, and every way it can be denied.
  toolDispatch: `flowchart TD
    calls["tool calls from the LLM reply"] --> validate{"known tool,<br/>valid JSON arguments?"}
    validate -->|no| denied["denied<br/>tool not found / invalid JSON"]
    validate -->|yes| dup{"duplicate of an identical call<br/>in the last assistant turn?"}
    dup -->|yes| denied
    dup -->|no| policy{"PolicyStore level<br/>read / write / yolo"}
    policy -->|read mode blocks it| denied
    policy -->|needs approval| gate{"approval gate"}
    gate -->|denied| denied
    gate -->|allowed| exec
    policy -->|allowed| exec["execute in parallel via std::async"]
    exec --> collect["collect results"]
    collect --> ordered["append in original order<br/>exactly one tool-role message per call"]
    denied --> ordered
    ordered --> envelope["feedback envelope<br/>status ok / error / denied"]`,

  // The compression cycle, and why it costs no full prefill.
  compressionPipeline: `sequenceDiagram
    autonumber
    participant A as Agent
    participant C as Context (live)
    participant L as LLM

    A->>C: read the snapshot
    C-->>A: messages (Context stays untouched)
    A->>A: collapse consecutive tool-call loops

    Note over A,L: both requests share a content-identical prefix,<br/>so the second extends the first's KV cache

    A->>L: classify request
    L-->>A: classification json - core / context / prune
    A->>L: extract request, replaying the classify prefix
    L-->>A: extraction json - memories + skills

    A->>A: apply classification, enforce headroom
    A->>C: clear() then push() the compressed set
    Note over A,C: the rebuild is the only live-context change,<br/>and only on success; a failure leaves it untouched`,

  // Context is a pure stack with a hash chain.
  contextHashChain: `flowchart TB
    subgraph ops["the only mutation operations"]
        push["push(msg): seal and append<br/>h = FNV(prev_hash, msg)"]
        pop["pop(): remove the top, LIFO<br/>restores the previous hash in O(1)"]
        clear["clear(): remove all<br/>used only by the compression rebuild"]
        get["get_all(): read-only view<br/>recomputes and asserts the chain"]
    end

    push --> chain["parallel deque of hashes"]
    pop --> chain
    clear --> chain
    chain --> verify["any in-place mutation breaks a link<br/>and asserts in a debug build"]
    get --> verify
    verify --> owner["single ownership, not locking<br/>one writer thread: everyone else reads snapshots"]`,

  // Injected blocks are assembled after the gate, never before.
  promptAssembly: `flowchart TB
    ctx["sealed Context, read-only"] --> gate{"compression gate"}
    gate -->|compressed| rebuild["context rebuilt<br/>clear + push"]
    gate -->|kept| copy
    rebuild --> copy["prompt copy<br/>the Context itself is never mutated"]
    copy --> head["head blocks<br/>inserted right after the system prompt"]
    copy --> tail["tail blocks<br/>appended after the conversation"]
    head --> order["within each: ascending prompt_priority<br/>ties keep insertion order"]
    tail --> order
    order --> sources["sources: retrieved memories · skill metadata<br/>session brief · plugin PromptBlockCapability"]
    sources --> omit["a block that renders empty is omitted"]
    sources --> isolated["a plugin block that throws<br/>cannot break the turn"]`,

  // Three failure modes, two responses each.
  errorRecovery: `flowchart TD
    each["evaluated every iteration"] --> tool{"3 or more identical<br/>tool call sets in a row?"}
    tool -->|yes| stopTool["hard stop: loop detected"]
    tool -->|no| text{"identical reply text<br/>repeating?"}
    text -->|second time| steerText["inject a steering message"]
    text -->|fifth time| stopText["hard stop"]
    text -->|no| fail{"consecutive tool failures"}
    fail -->|3| steerFail["inject a recovery steer"]
    fail -->|6| stopFail["hard stop: tool calls kept failing"]
    steerText --> hist["steer pushed to history_<br/>the model sees it on the next turn"]
    steerFail --> hist
    hist --> each`,

  // ------------------------------------------------------------ wire paths

  // Provider plugin to endpoint, with the port in the middle.
  llmDialectPath: `flowchart TB
    provider["provider plugin<br/>flavor + presets + optional dialect factory"] --> select["Config::flavor set at selection"]
    select --> make["make_dialect(flavor)<br/>resolved exactly once per client"]
    make -->|known flavor| dialect
    make -->|unknown flavor| fallback["openai fallback<br/>a typo never breaks a session"]
    fallback --> dialect["Dialect implementation<br/>endpoints · auth · body shape"]
    dialect --> request["build request body and headers"]
    request --> transport["HTTP transport via libcurl<br/>retry · timeout · cancel check"]
    transport --> endpoint["OpenAI-compatible endpoint"]
    endpoint --> decode["stream decoder: SSE framing"]
    decode --> chunks["StreamChunk per event → AgentHooks"]
    decode --> usage["TokenUsage mapping"]
    decode --> errors["is_retryable classification<br/>typed ApiError"]
    request --> normalize["tool calls normalized to the internal shape<br/>at the dialect edge, buffered and streamed alike"]`,

  // MCP session, from connect to shutdown.
  mcpSessionLifecycle: `sequenceDiagram
    autonumber
    participant U as User
    participant H as amber, the MCP host
    participant T as Transport
    participant S as MCP server

    U->>H: /mcp connect
    H->>T: open transport, stdio or streamable HTTP
    T->>S: spawn the process, or POST
    H->>S: initialize - protocol negotiation
    S-->>H: server capabilities
    H->>S: tools/list
    S-->>H: tools, paginated
    H->>S: resources/list, prompts/list
    S-->>H: resources, prompts
    Note over H,S: adapters map all three onto the Tool port
    S-->>H: notifications/listChanged
    U->>H: invoke a server tool
    H->>S: tools/call
    S-->>H: result
    U->>H: disconnect
    H->>S: shutdown`,

  // ----------------------------------------------------------------- safety

  // Mode, policy, approval, and the fail-safe.
  approvalGate: `flowchart TD
    call["tool call"] --> mode{"agent mode"}
    mode -->|read| block["read mode blocks<br/>every non-read-only tool"]
    mode -->|write or yolo| classify{"shell_classify<br/>bash only"}
    classify -->|read-only, or benign in-workspace write| run["runs free"]
    classify -->|destructive, or outside the workspace| policy
    classify -->|not a bash call| policy{"stored or session rule?"}
    policy -->|granted| run
    policy -->|no rule| handler{"approval handler installed?"}
    handler -->|no handler| deny["denied: fail-safe"]
    handler -->|non-TTY with no --yes| deny
    handler -->|TTY| dialog["approval dialog<br/>prompted once per scope: kind or folder"]
    dialog -->|approve| grant["session grant recorded"]
    grant --> run["tool executes<br/>own process group, stdin to /dev/null"]
    dialog -->|deny| deny
    block --> deny`,

  // Purely lexical confinement, before any I/O.
  pathConfinement: `flowchart TD
    input["path from the tool call"] --> empty{"empty?"}
    empty -->|yes| reject
    empty -->|no| root["resolve the root<br/>cached, then $AMBER_WORKSPACE, then cwd"]
    root --> normalize["lexical normalisation<br/>no filesystem access during resolution"]
    normalize --> escape{"escapes the root?<br/>dot-dot traversal or an absolute path outside"}
    escape -->|yes| reject["rejected"]
    escape -->|no| prefix{"slash-terminated prefix match<br/>/ws2 is not inside /ws"}
    prefix -->|no| reject
    prefix -->|yes| ok["confined absolute path returned"]
    ok --> io["only now does any file I/O happen"]`,

  // ------------------------------------------------------- other subsystems

  // Serial vs parallel sub-agents.
  subagentExecution: `flowchart TB
    task["task tool call"] --> mode{"execution mode"}
    mode -->|serial| serial["one at a time<br/>sequential requests share the system-prompt prefix<br/>so provider prompt caches stay warm"]
    mode -->|parallel| parallel["concurrent, capped by max()"]
    serial --> build
    parallel --> build["each sub-agent gets its own Context<br/>and a custom system prompt"]
    build --> iters["max_iterations cap, default 20"]
    iters --> result["result returned to the parent agent"]`,

  // Search is a port with two backends.
  searchBackendSelection: `flowchart TD
    query["search tool call<br/>mode argument"] --> pick{"mode"}
    pick -->|semantic| sem["SemanticBackend<br/>dependency-free lexical index<br/>cached per root and glob"]
    pick -->|anything else| grep["GrepBackend<br/>wraps grep -rnI, stateless"]
    sem --> fresh["a backend is created fresh per execute()"]
    grep --> fresh
    fresh --> format["results formatted into the tool envelope"]`,

  // The benchmark is a third client, outside the engine.
  benchHarness: `flowchart TB
    scenario["scenario JSON + static templates"] --> loader["scenario loader and validator"]
    loader --> runner["runner<br/>workspace setup → build Agent → run → teardown"]
    runner --> agent["Agent: the thing under test"]
    agent --> recorder["BenchmarkHooks : AgentHooks<br/>observes, never interferes"]
    recorder --> events["event stream"]
    events --> oracle["oracle matcher<br/>ordered or unordered steps, wildcards"]
    events --> resources["resource sampling<br/>RSS, CPU, baseline"]
    templates["static-template engine<br/>hidden tests + structural checks"] --> kpi
    oracle --> kpi["KPI aggregation"]
    resources --> kpi
    kpi --> report["reporters: text · JSON · markdown"]
    fake["FakeLLMClient<br/>hermetic, deterministic, CI-safe"] -.-> agent
    live["live mode<br/>any OpenAI-compatible endpoint"] -.-> agent`,

  // Learning happens only inside the compression cycle.
  memoryExtraction: `flowchart TD
    cycle["compression cycle"] --> response["CompressionResponse<br/>segments · memory_ops · skill_ops"]
    response --> apply["apply_compression_result"]
    apply --> mem["apply_memory_ops<br/>upsert, evidence starts at promote_threshold"]
    apply --> skills["apply_skill_ops<br/>upsert or deprecate, with a trigger phrase"]
    mem --> store
    skills --> store["JsonMemoryStore"]
    store --> dedup["dedup by content hash<br/>a repeat upsert just bumps evidence"]
    store --> recall["MemoryRetriever builds the system-prompt suffix<br/>recall is injected into the prompt copy"]`,
} as const;

export type DiagramName = keyof typeof diagrams;

// Gallery index. Every diagram above must appear here or the gallery page
// fails the build, so the two cannot drift.
export interface DiagramEntry {
  name: DiagramName;
  title: string;
  caption: string;
  section: string;
}

export const diagramCatalog: DiagramEntry[] = [
  {
    name: 'runtimeOverview',
    title: 'Runtime overview',
    caption: 'The microkernel: hosts above, plugin contributions installed into the core registries through the ledger.',
    section: 'Runtime',
  },
  {
    name: 'buildLayering',
    title: 'Build layering',
    caption: 'Two static libraries, three clients, and the process boundary external plugins live behind.',
    section: 'Runtime',
  },
  {
    name: 'pluginLifecycle',
    title: 'Plugin lifecycle',
    caption: 'Registered, Active, Deactivated, Failed, and what activation and deactivation actually do.',
    section: 'Plugins',
  },
  {
    name: 'capabilityLedger',
    title: 'Capability ledger',
    caption: 'Declare, install, record; then shutdown and unwind in reverse install order.',
    section: 'Plugins',
  },
  {
    name: 'pluginTierChoice',
    title: 'Which plugin tier',
    caption: 'Where a plugin runs is an isolation decision, not a size decision.',
    section: 'Plugins',
  },
  {
    name: 'pluginCommandBinding',
    title: 'Command binding',
    caption: 'The host derives the action and resolves the handler from the live registry at dispatch time.',
    section: 'Plugins',
  },
  {
    name: 'externalPluginHandshake',
    title: 'External plugin handshake',
    caption: 'Discovery, spawn on the first tool call, the version handshake, and the shutdown notification.',
    section: 'Plugins',
  },
  {
    name: 'commandTreeStructure',
    title: 'Command tree structure',
    caption: 'completions.json is the single source of truth; namespaces are keyed by full display path.',
    section: 'Command system',
  },
  {
    name: 'commandTreeDispatch',
    title: 'Command dispatch',
    caption: 'One keypress to one closure. The tree decides the action; the handler never names a command.',
    section: 'Command system',
  },
  {
    name: 'commandFeeds',
    title: 'Command feeds',
    caption: 'Dynamic values arrive as merged feed leaves, never as hardcoded C++ lists.',
    section: 'Command system',
  },
  {
    name: 'agentLoopCycle',
    title: 'Agent loop',
    caption: 'One turn of the ReAct loop: gate, assemble, call, dispatch, confirm.',
    section: 'Engine',
  },
  {
    name: 'toolDispatch',
    title: 'Tool dispatch',
    caption: 'Every tool call, and every way it can be denied, ending in one ordered envelope per call.',
    section: 'Engine',
  },
  {
    name: 'compressionPipeline',
    title: 'Compression pipeline',
    caption: 'Classify then extract, sharing one prefix, and why the cycle costs no full prefill.',
    section: 'Engine',
  },
  {
    name: 'contextHashChain',
    title: 'Context hash chain',
    caption: 'A pure stack: four operations, a hash chain, and single ownership instead of locking.',
    section: 'Engine',
  },
  {
    name: 'promptAssembly',
    title: 'Prompt assembly',
    caption: 'Injected blocks are assembled after the compression gate, never before it.',
    section: 'Engine',
  },
  {
    name: 'errorRecovery',
    title: 'Error recovery',
    caption: 'Three failure modes, two responses each: steer first, hard stop if it persists.',
    section: 'Engine',
  },
  {
    name: 'llmDialectPath',
    title: 'Provider wire path',
    caption: 'Provider plugin to endpoint, with the Dialect port resolving exactly once in the middle.',
    section: 'Wire',
  },
  {
    name: 'mcpSessionLifecycle',
    title: 'MCP session lifecycle',
    caption: 'Connect, negotiate, discover, adapt, invoke, shut down.',
    section: 'Wire',
  },
  {
    name: 'approvalGate',
    title: 'Approval gate',
    caption: 'Mode, policy and approval, ending in a fail-safe deny when no approver exists.',
    section: 'Safety',
  },
  {
    name: 'pathConfinement',
    title: 'Path confinement',
    caption: 'Purely lexical, checked before any file I/O, with slash-terminated prefix matching.',
    section: 'Safety',
  },
  {
    name: 'subagentExecution',
    title: 'Sub-agent execution',
    caption: 'Serial for cache warmth, parallel for throughput: each with its own Context.',
    section: 'Subsystems',
  },
  {
    name: 'searchBackendSelection',
    title: 'Search backends',
    caption: 'One port, two backends, and a grep fallback for anything unrecognised.',
    section: 'Subsystems',
  },
  {
    name: 'memoryExtraction',
    title: 'Memory extraction',
    caption: 'Learning happens only inside the compression cycle, driven by the model\u2019s judgment.',
    section: 'Subsystems',
  },
  {
    name: 'benchHarness',
    title: 'Benchmark harness',
    caption: 'A third client outside the engine: observe, score, aggregate, report.',
    section: 'Subsystems',
  },
  {
    name: 'testLayers',
    title: 'Test layers',
    caption: 'The four layers a plugin contribution has to survive, and the redirection guard.',
    section: 'Subsystems',
  },
];

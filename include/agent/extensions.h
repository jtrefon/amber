#ifndef AGENT_EXTENSIONS_H
#define AGENT_EXTENSIONS_H

// Contribution registries (spec §4).
//
// Each registry is a narrow interface with one job, and every accepted
// contribution is tagged with its owner (the plugin id) so the runtime's
// ledger can remove exactly what a plugin added and nothing else. Nothing here
// is a rendering concern: the host pulls from these registries when it composes
// a frame or a prompt.

#include "agent/config.h"
#include "agent/event_bus.h"
#include "agent/plugin_capability.h"
#include "agent/ui_services.h"
#include "agent/providers.h"
#include "agent/registry.h"
#include "agent/tool.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agent {

// One row of the introspection view: what exists, who put it there, and enough
// detail for the registry console to be useful without knowing the registry's
// type.
struct ExtensionItem {
    CapabilityKind kind = CapabilityKind::Tool;
    std::string owner; // plugin id, empty for host contributions
    std::string name;
    std::string detail; // registry-specific, human-readable
};

// ---------------------------------------------------------------------------
// Prompt blocks
// ---------------------------------------------------------------------------

// Where a contributed block lands in the prompt. Position is a property of
// what the block *is*, not of when it was registered:
//
// - `System` — concatenated into the system prompt itself. This is the stable
//   prefix the server caches, so it is where text that describes the harness
//   belongs: tool documentation, conventions. Content here is byte-identical
//   for identical inputs, and disabling its contributor removes it from the
//   prompt in the same action that removes the capability it documents.
// - `Head` — its own system message, immediately after the system prompt.
//   Reads as instructions for this request.
// - `Tail` — its own system message, at the end of the conversation. The
//   default, because a block that changes per turn is cheapest there: it costs
//   the KV cache only from its own position onwards.
enum class PromptPlacement : std::uint8_t { System, Head, Tail };

// A block of text contributed to the prompt. Rendering happens on the prompt
// copy, never on the sealed Context.
//
// Determinism: within a placement, blocks are ordered by (priority,
// registration order) and must render identically for identical inputs. A
// block whose content changes every turn invalidates the server's KV prefix
// from its position onwards, so volatile content belongs at a high priority
// (near the tail) - or should wait until the fact it reports actually changed.
class PromptRegistry {
public:
    using Render = std::function<std::string()>;

    Contribution add(const std::string& owner, const std::string& id, int priority, Render render,
                     PromptPlacement placement = PromptPlacement::Tail);

    // Blocks in priority order, skipping any that render empty.
    std::vector<std::string> render_all(PromptPlacement placement = PromptPlacement::Tail) const;

    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return blocks_.size(); }

private:
    struct Block {
        std::string owner;
        std::string id;
        int priority = 0;
        PromptPlacement placement = PromptPlacement::Tail;
        std::size_t seq = 0;
        Render render;
    };
    std::vector<Block> blocks_;
    std::size_t next_seq_ = 0;
};

// ---------------------------------------------------------------------------
// Status bar segments
// ---------------------------------------------------------------------------

// Semantic colour of a segment. The host owns the palette, so a segment never
// names a terminal colour (and a plugin never learns what a colour pair is).
enum class StatusTone : std::uint8_t {
    Dim,    // secondary information
    Good,   // healthy / active
    Warn,   // needs attention
    Crit,   // at a limit
    Accent, // highlighted state the user chose
    Banner, // leading identity tag
};

struct StatusText {
    std::string text;
    StatusTone tone = StatusTone::Dim;
};

// How an MCP server appears on the bar.
struct StatusMcpServer {
    std::string name;
    bool connected = false;
    bool has_error = false;
};

// What the host publishes about itself each frame. Segments are pure functions
// of this snapshot: they are called during frame composition, so they must be
// fast, allocation-light, and must not touch plugin state or do I/O.
struct StatusSnapshot {
    int window_index = 0; // 1-based, as displayed
    int window_count = 1;
    std::string model;
    std::string reasoning_effort;
    AgentMode mode = AgentMode::Read;
    bool scroll_mode = false;
    long latency_ms = -1;    // < 0: not measured yet
    double tps = -1.0;       // < 0: not measured yet
    long prompt_tokens = -1; // < 0: not reported
    long completion_tokens = -1;
    int running_jobs = 0;
    int job_seconds_left = -1;
    std::string running_tool;
    std::vector<StatusMcpServer> mcp_servers;
};

// A rendered segment, in display order.
struct StatusSegment {
    std::string id;
    std::string text;
    StatusTone tone = StatusTone::Dim;
    int drop_priority = 0; // higher drops first when the bar is too narrow
};

// The status bar is composed from these, never from a hardcoded list: amber's
// own segments register here with the host as their owner, so a plugin's
// segment and a core segment are the same kind of thing.
class StatusRegistry {
public:
    using Render = std::function<StatusText(const StatusSnapshot&)>;

    Contribution add(const std::string& owner, const std::string& id, int priority,
                     int drop_priority, Render render);

    // Segments that produced text, in (priority, registration) order.
    std::vector<StatusSegment> render(const StatusSnapshot& snapshot) const;

    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::string owner;
        std::string id;
        int priority = 0;
        int drop_priority = 0;
        std::size_t seq = 0;
        Render render;
    };
    std::vector<Entry> entries_;
    std::size_t next_seq_ = 0;
};

// ---------------------------------------------------------------------------
// Host services
// ---------------------------------------------------------------------------

// Host-owned runtime dependencies that a capability may need in order to
// construct what it contributes. Registries are not here — those are the
// runtime's own; these are the services the *host* owns (a tool binds to the
// job service, the todo store, the sub-agent executor, the cancel token),
// passed in by pointer so the runtime never takes ownership of host state.
//
// A capability whose factory needs none of this ignores the struct entirely;
// the pointers are null until the host attaches them.
struct HostServices {
    class JobService* jobs = nullptr;
    class TodoStore* todos = nullptr;
    class SubAgentExecutor* subagents = nullptr;
    const struct CancellationToken* cancel_token = nullptr;
};

// ---------------------------------------------------------------------------
// Wallet (the active provider's account state)
// ---------------------------------------------------------------------------

// One metered window: a rolling 5-hour session, a weekly cap, a monthly credit
// pool, a per-model quota - any limit with a reset. `percent_used` is 0-100
// (-1 = unknown). `remaining` and `entitlement` are counts or credits
// (-1 = not applicable). `resets_at` is ISO 8601 or empty.
struct WalletWindow {
    std::string label;
    double percent_used = -1;
    std::string resets_at;
    double remaining = -1;
    double entitlement = -1;
};

// What a provider reports about the account behind the active key: a plan
// name, the windows the provider meters, an optional prepaid balance, and the
// unit/currency the numbers are in. Every field is optional because providers
// answer the question "what is left?" in different shapes - a prepaid balance,
// a set of quota windows, or both.
//
// One snapshot type for one question. It used to be two mechanisms (a bare
// `optional<double>` "wallet" and a richer "allowance") that differed only in
// how much of this struct they filled; two registries, two flags, two status
// segments and two command pairs maintained for one concept is the
// inconsistency this type exists to prevent.
struct WalletSnapshot {
    std::string plan;
    std::vector<WalletWindow> windows;
    std::optional<double> credits_balance;
    std::string unit;
    std::string currency;

    // The common case: a provider that knows a balance and nothing else.
    static WalletSnapshot of_balance(double amount, std::string currency = "$");
};

// A provider supplies only the fetch; the runtime owns when to poll, how to
// cache it, and how it renders, so every provider's readout behaves the same
// and no provider hand-rolls a poll loop and a cache.
//
// The fetch returns the snapshot, or nullopt when there is nothing honest to
// report (no token configured, endpoint unreachable, rejected key). Returning
// nullopt is normal, not an error: the bar shows the unavailable state.
class WalletRegistry {
public:
    using Fetch = std::function<std::optional<WalletSnapshot>(const Config&)>;

    Contribution add(const std::string& owner, Fetch fetch);

    // The fetch for `owner` (a provider id), or null when it declares none.
    const Fetch* find(const std::string& owner) const;

    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<std::pair<std::string, Fetch>> entries_; // owner -> fetch
};

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

// A full-screen view a plugin (or the core) contributes: a title and a list of
// lines. The host owns placement, framing, scrolling and key routing — a panel
// only produces text and, optionally, consumes the keys it cares about.
struct PanelSpec {
    std::string id;
    std::string title;
    // Lines to display, given the width the host can offer. Called on every
    // repaint: pure, fast, no I/O.
    std::function<std::vector<std::string>(int width)> lines;
    // First refusal on every key while the panel is focused; return true when
    // the key was consumed. Optional: with none, the host's own keys apply.
    std::function<bool(int key)> on_key;
};

// Panels in registration order; the console is registered first so a bare
// "open a panel" always lands somewhere useful.
class PanelRegistry {
public:
    Contribution add(const std::string& owner, PanelSpec spec);

    const PanelSpec* find(const std::string& id) const;
    std::vector<PanelSpec> all() const;

    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::string owner;
        PanelSpec spec;
        std::size_t seq = 0;
    };
    std::vector<Entry> entries_;
    std::size_t next_seq_ = 0;
};

// ---------------------------------------------------------------------------
// Commands (slash-command namespaces)
// ---------------------------------------------------------------------------

// A plugin's slash-command namespace: a root name, its help/man, the child
// nodes (completions.json shape), and one handler per executable leaf.
//
// The plugin never names an action string: the host derives the action from the
// plugin id and the leaf's dotted path (`plugin.<id>.<path>`), so a plugin
// command cannot collide with a core one. A leaf with no handler stays inert:
// documented in the drawer, not executable. The same behaviour external plugin
// commands have today.
//
// Handlers return the text to display and the host prints it. That keeps the
// plugin UI-free (no tui/ includes), which is the framework's core rule.
struct CommandSpec {
    std::string root;       // namespace root, e.g. "hello"
    std::string help;       // drawer one-liner for the root
    std::string man;        // manual page for the root
    nlohmann::json subtree; // children nodes (completions.json shape)
    // Leaf path (dotted, relative to the root) -> handler returning output text.
    std::map<std::string, std::function<std::string(const std::string&)>> handlers;
};

class CommandRegistry {
public:
    struct Entry {
        std::string owner; // plugin id
        CommandSpec spec;
    };

    Contribution add(const std::string& owner, CommandSpec spec);

    // Every contributed namespace, for the host to merge and bind, in
    // registration order.
    const std::vector<Entry>& all() const noexcept { return entries_; }

    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// The services a capability installs into
// ---------------------------------------------------------------------------

// Passed to Capability::install(). Holds only what the harness owns; a
// capability never constructs its own registry.
class PluginServices {
public:
    PluginServices(ToolRegistry& tools, PromptRegistry& prompts, StatusRegistry& status,
                   PanelRegistry& panels, WalletRegistry& wallets,
                   EventBus& events, CommandRegistry& commands) noexcept;

    ToolRegistry& tools() noexcept { return *tools_; }
    PromptRegistry& prompts() noexcept { return *prompts_; }
    StatusRegistry& status() noexcept { return *status_; }
    PanelRegistry& panels() noexcept { return *panels_; }
    WalletRegistry& wallets() noexcept { return *wallets_; }
    EventBus& events() noexcept { return *events_; }
    CommandRegistry& commands() noexcept { return *commands_; }

    // The plugin whose capabilities are being installed right now. The runtime
    // sets this around each plugin's install pass so a contribution is tagged
    // with its owner without the capability knowing about the runtime.
    const std::string& owner() const noexcept { return owner_; }
    void set_owner(std::string owner) { owner_ = std::move(owner); }

    // The host's live configuration. Null before the host attaches one (tests
    // and headless hosts that do not need it).
    const Config* config = nullptr;

    // The user-interaction port. Never null: an unattached host leaves the
    // fail-closed null implementation in place, so a capability can ask
    // without checking first (see ui_services.h).
    UiServices* ui = &null_ui_services();

    // Host-owned services a capability may need to build what it contributes.
    // Null until the host attaches them; a capability that needs none ignores
    // this.
    const HostServices* host = nullptr;

private:
    ToolRegistry* tools_;
    PromptRegistry* prompts_;
    StatusRegistry* status_;
    PanelRegistry* panels_;
    WalletRegistry* wallets_;
    EventBus* events_;
    CommandRegistry* commands_;
    std::string owner_;
};

// ---------------------------------------------------------------------------
// Concrete capabilities a plugin declares
// ---------------------------------------------------------------------------

// Installs tools under the plugin's ownership; removal takes exactly those
// tools out again.
//
// A tool arrives as a *factory* that receives the harness services. A tool is
// not pure data — the bash tool binds to the job service, todowrite to the todo
// store, task to the sub-agent executor — and the factory is also what makes the
// capability re-installable: disabling and re-enabling a plugin replays the
// declaration, so a capability that hands over a finished object would have
// nothing left to install the second time.
//
// The factory returns a list: the process tools are several tools that share
// one binding, and an empty list means the capability declined (a tool gated on
// configuration is simply absent, not an error).
class ToolCapability : public Capability {
public:
    using Factory = std::function<std::vector<std::unique_ptr<Tool>>(PluginServices&)>;

    // What each contributed tool is and does, keyed by tool name. One entry per
    // tool: the verb renders a status line, the role lets the audit see the
    // shape of the enabled set. A tool left out falls back to a generic word
    // and the Other role, so a plugin that declares nothing is still valid - it
    // is simply invisible to the audit.
    using Meta = std::map<std::string, ToolMeta>;

    ToolCapability(std::string name, Factory factory, Meta meta = {});
    std::string name() const override { return name_; }
    CapabilityKind kind() const override { return CapabilityKind::Tool; }
    InstallResult install(PluginServices& services) override;

private:
    std::string name_;
    Meta meta_;
    Factory factory_;
};

// Installs one ordered prompt block.
class PromptBlockCapability : public Capability {
public:
    PromptBlockCapability(std::string id, int priority, PromptRegistry::Render render,
                          PromptPlacement placement = PromptPlacement::Tail);
    std::string name() const override { return id_; }
    CapabilityKind kind() const override { return CapabilityKind::PromptBlock; }
    InstallResult install(PluginServices& services) override;

private:
    std::string id_;
    int priority_;
    PromptRegistry::Render render_;
    PromptPlacement placement_;
};

// Contributes a provider: its presets, and optionally the wire protocol they
// speak (spec §7).
//
// The dialect factory (when given) registers into the same table the built-ins
// use; the presets become provider rows through the ordinary repository merge,
// so a plugin provider appears in `/provider list` and the command feed with no
// new concept. Wire behaviour lives entirely in the dialect - this capability
// only carries data and the factory.
//
// A provider that speaks a shared protocol (an OpenAI-compatible gateway)
// passes no factory: it contributes presets and leaves the dialect alone, so
// switching it off cannot take the shared protocol down with it.
class ProviderCapability : public Capability {
public:
    struct Preset {
        std::string name; // provider name, e.g. "gemini"
        std::string api_base;
        std::string default_model;
        bool requires_key = true;
    };

    // `make_dialect` empty: presets only, speaking a protocol provided
    // elsewhere (the shared openai dialect).
    ProviderCapability(std::string flavor,
                       std::function<std::unique_ptr<class Dialect>()> make_dialect = {},
                       std::vector<Preset> presets = {});
    ~ProviderCapability() override;

    std::string name() const override { return flavor_; }
    CapabilityKind kind() const override { return CapabilityKind::Provider; }
    InstallResult install(PluginServices& services) override;

private:
    std::string flavor_;
    std::function<std::unique_ptr<class Dialect>()> make_dialect_;
    std::vector<Preset> presets_;
};

// Contributes one status-bar segment.
class StatusSegmentCapability : public Capability {
public:
    StatusSegmentCapability(std::string id, int priority, int drop_priority,
                            StatusRegistry::Render render);
    std::string name() const override { return id_; }
    CapabilityKind kind() const override { return CapabilityKind::StatusSegment; }
    InstallResult install(PluginServices& services) override;

private:
    std::string id_;
    int priority_;
    int drop_priority_;
    StatusRegistry::Render render_;
};

// Declares how to fetch this plugin's wallet (usually a provider's balance).
// Install/unwind only register and remove the fetch; polling and rendering are
// the runtime's.
class WalletCapability : public Capability {
public:
    explicit WalletCapability(WalletRegistry::Fetch fetch);
    std::string name() const override { return "wallet"; }
    CapabilityKind kind() const override { return CapabilityKind::Wallet; }
    InstallResult install(PluginServices& services) override;

private:
    WalletRegistry::Fetch fetch_;
};

// Contributes a full-screen panel.
class PanelCapability : public Capability {
public:
    PanelCapability(PanelSpec spec);
    std::string name() const override { return spec_.id; }
    CapabilityKind kind() const override { return CapabilityKind::Panel; }
    InstallResult install(PluginServices& services) override;

private:
    PanelSpec spec_;
};

// Contributes a slash-command namespace. The plugin declares the root, its
// help/man, the child nodes, and one handler per executable leaf; the host
// merges the subtree and binds the handlers. See CommandSpec for the contract.
class CommandCapability : public Capability {
public:
    explicit CommandCapability(CommandSpec spec);
    std::string name() const override { return spec_.root; }
    CapabilityKind kind() const override { return CapabilityKind::Command; }
    InstallResult install(PluginServices& services) override;

private:
    CommandSpec spec_;
};

} // namespace agent

#endif // AGENT_EXTENSIONS_H

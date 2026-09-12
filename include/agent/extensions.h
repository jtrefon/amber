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

// A block of text appended to the prompt as its own system message. Rendering
// happens on the prompt copy, never on the sealed Context.
//
// Determinism: blocks are ordered by (priority, registration order) and must
// render identically for identical inputs. A block whose content changes every
// turn invalidates the server's KV prefix from its position onwards, so
// volatile content belongs at a high priority (near the tail) - or should wait
// until the fact it reports actually changed.
class PromptRegistry {
public:
    using Render = std::function<std::string()>;

    Contribution add(const std::string& owner, const std::string& id, int priority, Render render);

    // Blocks in priority order, skipping any that render empty.
    std::vector<std::string> render_all() const;

    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return blocks_.size(); }

private:
    struct Block {
        std::string owner;
        std::string id;
        int priority = 0;
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
// Wallets
// ---------------------------------------------------------------------------

// A provider's account balance, for the status bar. A plugin supplies only the
// fetch; the runtime owns when to poll, how to cache it, and how it renders, so
// every provider's readout behaves the same and no provider hand-rolls a poll
// loop and a cache.
//
// The fetch returns the amount in the display currency, or nullopt when there
// is nothing honest to report (no token configured, endpoint unreachable,
// rejected key). Returning nullopt is normal, not an error: the bar shows the
// unavailable state.
class WalletRegistry {
public:
    using Fetch = std::function<std::optional<double>(const Config&)>;

    Contribution add(const std::string& owner, Fetch fetch);

    // The fetch for `owner` (a provider id), or null when it declares none.
    const Fetch* find(const std::string& owner) const;

    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<std::pair<std::string, Fetch>> entries_; // owner -> fetch
};

// ---------------------------------------------------------------------------
// Allowance (subscription/quota windows)
// ---------------------------------------------------------------------------

// One usage window: a rolling 5-hour session, a weekly cap, a monthly credit
// pool, a per-model quota — any metered limit with a reset. `percent_used` is
// 0–100 (-1 = unknown). `remaining` and `entitlement` are counts or credits
// (-1 = not applicable). `resets_at` is ISO 8601 or empty.
struct AllowanceWindow {
    std::string label;
    double percent_used = -1;
    std::string resets_at;
    double remaining = -1;
    double entitlement = -1;
};

// The full allowance picture for a provider: a plan name, the windows the
// provider reports, an optional prepaid credit balance, and the unit/currency
// the numbers are in.
struct AllowanceSnapshot {
    std::string plan;
    std::vector<AllowanceWindow> windows;
    std::optional<double> credits_balance;
    std::string unit;
    std::string currency;
};

// Same shape as WalletRegistry: a provider supplies a fetch, the runtime owns
// polling, caching, and rendering.
class AllowanceRegistry {
public:
    using Fetch = std::function<std::optional<AllowanceSnapshot>(const Config&)>;

    Contribution add(const std::string& owner, Fetch fetch);
    const Fetch* find(const std::string& owner) const;
    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<std::pair<std::string, Fetch>> entries_;
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
// Commands
// ---------------------------------------------------------------------------

// A slash-command subtree plus one handler per leaf. The host merges the
// subtree into its command tree and registers the handlers, so a contributed
// leaf always executes - the failure mode of the v1 tier, where subtrees
// rendered in the drawer but had no handler behind them.
class CommandRegistry {
public:
    using Handler = std::function<void(const std::string& arg)>;

    struct Node {
        std::string owner;
        std::string root;                        // namespace root this plugin owns
        std::string subtree_json;                // completions.json-shaped children
        std::map<std::string, Handler> handlers; // leaf path -> handler
    };

    Contribution add(const std::string& owner, const std::string& root,
                     const std::string& subtree_json, std::map<std::string, Handler> handlers);

    // Subtree for `root`, or an empty string when no plugin owns it.
    std::string subtree(const std::string& root) const;
    // Invoke the handler for `root` + `path` (slash-separated, no leading
    // root). Returns false when nothing is registered for that leaf.
    bool dispatch(const std::string& root, const std::string& path, const std::string& arg) const;

    std::vector<ExtensionItem> items() const;
    std::size_t size() const noexcept { return nodes_.size(); }

private:
    std::vector<Node> nodes_;
};

// ---------------------------------------------------------------------------
// Per-plugin settings
// ---------------------------------------------------------------------------

// Plugin-owned key/value state. The runtime reads and writes the backing file;
// plugins only ever see the map. Kept separate from the harness config so a
// disabled plugin's settings cannot leak into global configuration.
class PluginSettingsStore {
public:
    // Values for `owner`; empty when the plugin has none.
    std::map<std::string, std::string> get(const std::string& owner) const;
    std::string get(const std::string& owner, const std::string& key) const;
    void set(const std::string& owner, const std::string& key, const std::string& value);
    bool has(const std::string& owner) const;

    // Declare a key a plugin offers, with its one-line description. A
    // declaration is how the console can show a setting that is still unset.
    void declare(const std::string& owner, const std::string& key, const std::string& help);
    void undeclare(const std::string& owner, const std::string& key);

    std::vector<ExtensionItem> items() const;

private:
    std::map<std::string, std::map<std::string, std::string>> values_;
    std::map<std::string, std::map<std::string, std::string>> declared_;
};

// ---------------------------------------------------------------------------
// The services a capability installs into
// ---------------------------------------------------------------------------

// Passed to Capability::install(). Holds only what the harness owns; a
// capability never constructs its own registry.
class PluginServices {
public:
    PluginServices(ToolRegistry& tools, PromptRegistry& prompts, CommandRegistry& commands,
                   StatusRegistry& status, PanelRegistry& panels, WalletRegistry& wallets,
                   AllowanceRegistry& allowances, PluginSettingsStore& settings,
                   EventBus& events) noexcept;

    ToolRegistry& tools() noexcept { return *tools_; }
    PromptRegistry& prompts() noexcept { return *prompts_; }
    CommandRegistry& commands() noexcept { return *commands_; }
    StatusRegistry& status() noexcept { return *status_; }
    PanelRegistry& panels() noexcept { return *panels_; }
    WalletRegistry& wallets() noexcept { return *wallets_; }
    AllowanceRegistry& allowances() noexcept { return *allowances_; }
    PluginSettingsStore& settings() noexcept { return *settings_; }
    EventBus& events() noexcept { return *events_; }

    // The plugin whose capabilities are being installed right now. The runtime
    // sets this around each plugin's install pass so a contribution is tagged
    // with its owner without the capability knowing about the runtime.
    const std::string& owner() const noexcept { return owner_; }
    void set_owner(std::string owner) { owner_ = std::move(owner); }

    // The host's live configuration. Null before the host attaches one (tests
    // and headless hosts that do not need it).
    const Config* config = nullptr;

private:
    ToolRegistry* tools_;
    PromptRegistry* prompts_;
    CommandRegistry* commands_;
    StatusRegistry* status_;
    PanelRegistry* panels_;
    WalletRegistry* wallets_;
    AllowanceRegistry* allowances_;
    PluginSettingsStore* settings_;
    EventBus* events_;
    std::string owner_;
};

// ---------------------------------------------------------------------------
// Concrete capabilities a plugin declares
// ---------------------------------------------------------------------------

// Installs a tool under the plugin's ownership; removal takes exactly that
// tool out again.
class ToolCapability : public Capability {
public:
    ToolCapability(std::string name, std::unique_ptr<Tool> tool);
    std::string name() const override { return name_; }
    CapabilityKind kind() const override { return CapabilityKind::Tool; }
    InstallResult install(PluginServices& services) override;

private:
    std::string name_;
    std::unique_ptr<Tool> tool_;
};

// Installs a command subtree and its leaf handlers.
class CommandCapability : public Capability {
public:
    using Handler = CommandRegistry::Handler;

    CommandCapability(std::string root, std::string subtree_json,
                      std::map<std::string, Handler> handlers);
    std::string name() const override { return root_; }
    CapabilityKind kind() const override { return CapabilityKind::Command; }
    InstallResult install(PluginServices& services) override;

private:
    std::string root_;
    std::string subtree_json_;
    std::map<std::string, Handler> handlers_;
};

// Installs one ordered prompt block.
class PromptBlockCapability : public Capability {
public:
    PromptBlockCapability(std::string id, int priority, PromptRegistry::Render render);
    std::string name() const override { return id_; }
    CapabilityKind kind() const override { return CapabilityKind::PromptBlock; }
    InstallResult install(PluginServices& services) override;

private:
    std::string id_;
    int priority_;
    PromptRegistry::Render render_;
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

// Contributes an allowance fetch: the plugin supplies the fetch, the runtime
// owns polling, caching, and rendering. Same pattern as WalletCapability.
class AllowanceCapability : public Capability {
public:
    explicit AllowanceCapability(AllowanceRegistry::Fetch fetch);
    std::string name() const override { return "allowance"; }
    CapabilityKind kind() const override { return CapabilityKind::Allowance; }
    InstallResult install(PluginServices& services) override;

private:
    AllowanceRegistry::Fetch fetch_;
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

// Declares a setting key so the console can show it and the command tree can
// offer it; values live in the per-plugin store.
class SettingCapability : public Capability {
public:
    SettingCapability(std::string key, std::string help);
    std::string name() const override { return key_; }
    CapabilityKind kind() const override { return CapabilityKind::Setting; }
    InstallResult install(PluginServices& services) override;

private:
    std::string key_;
    std::string help_;
};

} // namespace agent

#endif // AGENT_EXTENSIONS_H

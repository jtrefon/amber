#ifndef AGENT_EXTENSIONS_H
#define AGENT_EXTENSIONS_H

// Contribution registries (spec §4).
//
// Each registry is a narrow interface with one job, and every accepted
// contribution is tagged with its owner (the plugin id) so the runtime's
// ledger can remove exactly what a plugin added and nothing else. Nothing here
// is a rendering concern: the host pulls from these registries when it composes
// a frame or a prompt.

#include "agent/event_bus.h"
#include "agent/plugin_capability.h"
#include "agent/registry.h"
#include "agent/tool.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace agent {

// One row of the introspection view: what exists, who put it there, and enough
// detail for the registry console to be useful without knowing the registry's
// type.
struct ExtensionItem {
    CapabilityKind kind = CapabilityKind::Tool;
    std::string owner;   // plugin id, empty for host contributions
    std::string name;
    std::string detail;  // registry-specific, human-readable
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

    Contribution add(const std::string& owner, const std::string& id, int priority,
                     Render render);

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
        std::string root;   // namespace root this plugin owns
        std::string subtree_json;  // completions.json-shaped children
        std::map<std::string, Handler> handlers;  // leaf path -> handler
    };

    Contribution add(const std::string& owner, const std::string& root,
                     const std::string& subtree_json,
                     std::map<std::string, Handler> handlers);

    // Subtree for `root`, or an empty string when no plugin owns it.
    std::string subtree(const std::string& root) const;
    // Invoke the handler for `root` + `path` (slash-separated, no leading
    // root). Returns false when nothing is registered for that leaf.
    bool dispatch(const std::string& root, const std::string& path,
                  const std::string& arg) const;

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
    void set(const std::string& owner, const std::string& key,
             const std::string& value);
    bool has(const std::string& owner) const;

    // Declare a key a plugin offers, with its one-line description. A
    // declaration is how the console can show a setting that is still unset.
    void declare(const std::string& owner, const std::string& key,
                 const std::string& help);
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
    PluginServices(ToolRegistry& tools, PromptRegistry& prompts,
                   CommandRegistry& commands, PluginSettingsStore& settings,
                   EventBus& events) noexcept;

    ToolRegistry& tools() noexcept { return *tools_; }
    PromptRegistry& prompts() noexcept { return *prompts_; }
    CommandRegistry& commands() noexcept { return *commands_; }
    PluginSettingsStore& settings() noexcept { return *settings_; }
    EventBus& events() noexcept { return *events_; }

    // The plugin whose capabilities are being installed right now. The runtime
    // sets this around each plugin's install pass so a contribution is tagged
    // with its owner without the capability knowing about the runtime.
    const std::string& owner() const noexcept { return owner_; }
    void set_owner(std::string owner) { owner_ = std::move(owner); }

private:
    ToolRegistry* tools_;
    PromptRegistry* prompts_;
    CommandRegistry* commands_;
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

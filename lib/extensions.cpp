#include "agent/extensions.h"

#include "agent/dialect.h"

#include <algorithm>
#include <utility>

namespace agent {

// ---------------------------------------------------------------------------
// PromptRegistry
// ---------------------------------------------------------------------------

Contribution PromptRegistry::add(const std::string& owner, const std::string& id,
                                 int priority, Render render) {
    Block block;
    block.owner = owner;
    block.id = id;
    block.priority = priority;
    block.seq = next_seq_++;
    block.render = std::move(render);
    blocks_.push_back(std::move(block));
    // Stable ordering: priority first, registration order to break ties, so
    // two plugins at the same priority never swap places between turns.
    std::stable_sort(blocks_.begin(), blocks_.end(),
                     [](const Block& a, const Block& b) {
                         if (a.priority != b.priority) return a.priority < b.priority;
                         return a.seq < b.seq;
                     });

    Contribution c;
    c.kind = CapabilityKind::PromptBlock;
    c.name = id;
    c.remove = [this, owner, id] {
        blocks_.erase(std::remove_if(blocks_.begin(), blocks_.end(),
                                     [&](const Block& b) {
                                         return b.owner == owner && b.id == id;
                                     }),
                      blocks_.end());
    };
    return c;
}

std::vector<std::string> PromptRegistry::render_all() const {
    std::vector<std::string> out;
    for (const auto& block : blocks_) {
        if (!block.render) continue;
        std::string text = block.render();
        if (!text.empty()) out.push_back(std::move(text));
    }
    return out;
}

std::vector<ExtensionItem> PromptRegistry::items() const {
    std::vector<ExtensionItem> out;
    out.reserve(blocks_.size());
    for (const auto& block : blocks_) {
        out.push_back({CapabilityKind::PromptBlock, block.owner, block.id,
                       "priority " + std::to_string(block.priority)});
    }
    return out;
}

// ---------------------------------------------------------------------------
// StatusRegistry
// ---------------------------------------------------------------------------

Contribution StatusRegistry::add(const std::string& owner, const std::string& id,
                                 int priority, int drop_priority, Render render) {
    Entry entry;
    entry.owner = owner;
    entry.id = id;
    entry.priority = priority;
    entry.drop_priority = drop_priority;
    entry.seq = next_seq_++;
    entry.render = std::move(render);
    entries_.push_back(std::move(entry));
    // Priority first, registration order to break ties, so two segments at the
    // same priority never swap places between frames.
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) {
                         if (a.priority != b.priority) return a.priority < b.priority;
                         return a.seq < b.seq;
                     });

    Contribution c;
    c.kind = CapabilityKind::StatusSegment;
    c.name = id;
    c.remove = [this, owner, id] {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const Entry& e) {
                                          return e.owner == owner && e.id == id;
                                      }),
                       entries_.end());
    };
    return c;
}

std::vector<StatusSegment>
StatusRegistry::render(const StatusSnapshot& snapshot) const {
    std::vector<StatusSegment> out;
    for (const auto& entry : entries_) {
        if (!entry.render) continue;
        StatusText text = entry.render(snapshot);
        if (text.text.empty()) continue;   // a segment may decline to appear
        out.push_back({entry.id, std::move(text.text), text.tone,
                       entry.drop_priority});
    }
    return out;
}

std::vector<ExtensionItem> StatusRegistry::items() const {
    std::vector<ExtensionItem> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back({CapabilityKind::StatusSegment, entry.owner, entry.id,
                       "priority " + std::to_string(entry.priority)});
    }
    return out;
}

// ---------------------------------------------------------------------------
// CommandRegistry
// ---------------------------------------------------------------------------

Contribution CommandRegistry::add(const std::string& owner, const std::string& root,
                                  const std::string& subtree_json,
                                  std::map<std::string, Handler> handlers) {
    Node node;
    node.owner = owner;
    node.root = root;
    node.subtree_json = subtree_json;
    node.handlers = std::move(handlers);
    nodes_.push_back(std::move(node));

    Contribution c;
    c.kind = CapabilityKind::Command;
    c.name = root;
    c.remove = [this, owner, root] {
        nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                    [&](const Node& n) {
                                        return n.owner == owner && n.root == root;
                                    }),
                     nodes_.end());
    };
    return c;
}

std::string CommandRegistry::subtree(const std::string& root) const {
    for (const auto& node : nodes_) {
        if (node.root == root) return node.subtree_json;
    }
    return {};
}

bool CommandRegistry::dispatch(const std::string& root, const std::string& path,
                               const std::string& arg) const {
    for (const auto& node : nodes_) {
        if (node.root != root) continue;
        auto it = node.handlers.find(path);
        if (it == node.handlers.end() || !it->second) return false;
        it->second(arg);
        return true;
    }
    return false;
}

std::vector<ExtensionItem> CommandRegistry::items() const {
    std::vector<ExtensionItem> out;
    for (const auto& node : nodes_) {
        out.push_back({CapabilityKind::Command, node.owner, node.root,
                       std::to_string(node.handlers.size()) + " command(s)"});
    }
    return out;
}

// ---------------------------------------------------------------------------
// PluginSettingsStore
// ---------------------------------------------------------------------------

std::map<std::string, std::string>
PluginSettingsStore::get(const std::string& owner) const {
    auto it = values_.find(owner);
    return it == values_.end() ? std::map<std::string, std::string>{} : it->second;
}

std::string PluginSettingsStore::get(const std::string& owner,
                                     const std::string& key) const {
    auto owner_it = values_.find(owner);
    if (owner_it == values_.end()) return {};
    auto it = owner_it->second.find(key);
    return it == owner_it->second.end() ? std::string{} : it->second;
}

void PluginSettingsStore::set(const std::string& owner, const std::string& key,
                              const std::string& value) {
    values_[owner][key] = value;
}

bool PluginSettingsStore::has(const std::string& owner) const {
    return values_.find(owner) != values_.end();
}

void PluginSettingsStore::declare(const std::string& owner, const std::string& key,
                                  const std::string& help) {
    declared_[owner][key] = help;
}

void PluginSettingsStore::undeclare(const std::string& owner,
                                    const std::string& key) {
    auto it = declared_.find(owner);
    if (it == declared_.end()) return;
    it->second.erase(key);
    if (it->second.empty()) declared_.erase(it);
    values_[owner].erase(key);
    if (values_[owner].empty()) values_.erase(owner);
}

std::vector<ExtensionItem> PluginSettingsStore::items() const {
    std::vector<ExtensionItem> out;
    for (const auto& [owner, kv] : values_) {
        for (const auto& [key, value] : kv) {
            out.push_back({CapabilityKind::Setting, owner, key, value});
        }
    }
    for (const auto& [owner, kv] : declared_) {
        for (const auto& [key, help] : kv) {
            if (get(owner, key).empty())
                out.push_back({CapabilityKind::Setting, owner, key,
                               help.empty() ? "(unset)" : help});
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// PluginServices
// ---------------------------------------------------------------------------

PluginServices::PluginServices(ToolRegistry& tools, PromptRegistry& prompts,
                               CommandRegistry& commands, StatusRegistry& status,
                               PluginSettingsStore& settings,
                               EventBus& events) noexcept
    : tools_(&tools), prompts_(&prompts), commands_(&commands),
      status_(&status), settings_(&settings), events_(&events) {}

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

ToolCapability::ToolCapability(std::string name, std::unique_ptr<Tool> tool)
    : name_(std::move(name)), tool_(std::move(tool)) {}

InstallResult ToolCapability::install(PluginServices& services) {
    InstallResult r;
    if (!tool_) {
        r.error = "tool capability '" + name_ + "' has no tool";
        return r;
    }
    ToolRegistry* registry = &services.tools();
    const std::string registered = tool_->name();
    registry->register_tool(std::move(tool_));
    r.ok = true;
    r.contribution.kind = CapabilityKind::Tool;
    r.contribution.name = registered;
    r.contribution.remove = [registry, registered] {
        registry->remove_tool(registered);
    };
    return r;
}

CommandCapability::CommandCapability(std::string root, std::string subtree_json,
                                     std::map<std::string, Handler> handlers)
    : root_(std::move(root)), subtree_json_(std::move(subtree_json)),
      handlers_(std::move(handlers)) {}

InstallResult CommandCapability::install(PluginServices& services) {
    InstallResult r;
    if (root_.empty()) {
        r.error = "command capability needs a namespace root";
        return r;
    }
    r.contribution = services.commands().add(services.owner(), root_,
                                             subtree_json_, handlers_);
    r.ok = true;
    return r;
}

PromptBlockCapability::PromptBlockCapability(std::string id, int priority,
                                             PromptRegistry::Render render)
    : id_(std::move(id)), priority_(priority), render_(std::move(render)) {}

InstallResult PromptBlockCapability::install(PluginServices& services) {
    InstallResult r;
    if (!render_) {
        r.error = "prompt block '" + id_ + "' has no renderer";
        return r;
    }
    r.contribution = services.prompts().add(services.owner(), id_, priority_,
                                            render_);
    r.ok = true;
    return r;
}

ProviderCapability::ProviderCapability(
    std::string flavor,
    std::function<std::unique_ptr<class Dialect>()> make_dialect,
    std::vector<Preset> presets)
    : flavor_(std::move(flavor)), make_dialect_(std::move(make_dialect)),
      presets_(std::move(presets)) {}

// Out-of-line so the header needs only a forward declaration of Dialect.
ProviderCapability::~ProviderCapability() = default;

InstallResult ProviderCapability::install(PluginServices& services) {
    InstallResult r;
    if (flavor_.empty() || !make_dialect_) {
        r.error = "provider capability needs a flavor and a dialect factory";
        return r;
    }
    const std::string owner = services.owner();
    register_dialect(flavor_, make_dialect_, owner);
    for (const auto& preset : presets_) {
        Provider p;
        p.name = preset.name;
        p.api_base = preset.api_base;
        p.default_model = preset.default_model;
        p.requires_key = preset.requires_key;
        p.builtin = true;
        p.flavor = flavor_;
        register_provider_preset(p, owner);
    }

    r.ok = true;
    r.contribution.kind = CapabilityKind::Provider;
    r.contribution.name = flavor_;
    r.contribution.remove = [owner, flavor = flavor_] {
        // Take back exactly this plugin's contribution: its dialect (marked
        // unavailable, so a provider file pointing at it fails loudly) and its
        // preset rows.
        unregister_dialects_for(owner);
        unregister_provider_presets_for(owner);
    };
    return r;
}

StatusSegmentCapability::StatusSegmentCapability(std::string id, int priority,
                                                 int drop_priority,
                                                 StatusRegistry::Render render)
    : id_(std::move(id)), priority_(priority), drop_priority_(drop_priority),
      render_(std::move(render)) {}

InstallResult StatusSegmentCapability::install(PluginServices& services) {
    InstallResult r;
    if (id_.empty() || !render_) {
        r.error = "status segment capability needs an id and a renderer";
        return r;
    }
    r.contribution = services.status().add(services.owner(), id_, priority_,
                                           drop_priority_, render_);
    r.ok = true;
    return r;
}

SettingCapability::SettingCapability(std::string key, std::string help)
    : key_(std::move(key)), help_(std::move(help)) {}

InstallResult SettingCapability::install(PluginServices& services) {
    InstallResult r;
    if (key_.empty()) {
        r.error = "setting capability needs a key";
        return r;
    }
    const std::string owner = services.owner();
    PluginSettingsStore* store = &services.settings();
    store->declare(owner, key_, help_);
    r.ok = true;
    r.contribution.kind = CapabilityKind::Setting;
    r.contribution.name = key_;
    r.contribution.remove = [store, owner, key = key_] {
        store->undeclare(owner, key);
    };
    return r;
}

} // namespace agent

#include "agent/extensions.h"

#include "agent/dialect.h"

#include <algorithm>
#include <utility>

namespace agent {

// ---------------------------------------------------------------------------
// PromptRegistry
// ---------------------------------------------------------------------------

Contribution PromptRegistry::add(const std::string& owner, const std::string& id, int priority,
                                 Render render, PromptPlacement placement) {
    Block block;
    block.owner = owner;
    block.id = id;
    block.priority = priority;
    block.placement = placement;
    block.seq = next_seq_++;
    block.render = std::move(render);
    blocks_.push_back(std::move(block));
    // Stable ordering: priority first, registration order to break ties, so
    // two plugins at the same priority never swap places between turns.
    std::stable_sort(blocks_.begin(), blocks_.end(), [](const Block& a, const Block& b) {
        if (a.priority != b.priority)
            return a.priority < b.priority;
        return a.seq < b.seq;
    });

    Contribution c;
    c.kind = CapabilityKind::PromptBlock;
    c.name = id;
    c.remove = [this, owner, id] {
        blocks_.erase(
            std::remove_if(blocks_.begin(), blocks_.end(),
                           [&](const Block& b) { return b.owner == owner && b.id == id; }),
            blocks_.end());
    };
    return c;
}

std::vector<std::string> PromptRegistry::render_all(PromptPlacement placement) const {
    std::vector<std::string> out;
    for (const auto& block : blocks_) {
        if (block.placement != placement || !block.render)
            continue;
        std::string text = block.render();
        if (!text.empty())
            out.push_back(std::move(text));
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

Contribution StatusRegistry::add(const std::string& owner, const std::string& id, int priority,
                                 int drop_priority, Render render) {
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
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.priority != b.priority)
            return a.priority < b.priority;
        return a.seq < b.seq;
    });

    Contribution c;
    c.kind = CapabilityKind::StatusSegment;
    c.name = id;
    c.remove = [this, owner, id] {
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [&](const Entry& e) { return e.owner == owner && e.id == id; }),
            entries_.end());
    };
    return c;
}

std::vector<StatusSegment> StatusRegistry::render(const StatusSnapshot& snapshot) const {
    std::vector<StatusSegment> out;
    for (const auto& entry : entries_) {
        if (!entry.render)
            continue;
        StatusText text = entry.render(snapshot);
        if (text.text.empty())
            continue; // a segment may decline to appear
        out.push_back({entry.id, std::move(text.text), text.tone, entry.drop_priority});
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
// WalletRegistry
// ---------------------------------------------------------------------------

Contribution WalletRegistry::add(const std::string& owner, Fetch fetch) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const std::pair<std::string, Fetch>& e) { return e.first == owner; }),
        entries_.end());
    entries_.emplace_back(owner, std::move(fetch));

    Contribution c;
    c.kind = CapabilityKind::Wallet;
    c.name = owner;
    c.remove = [this, owner] {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const std::pair<std::string, Fetch>& e) {
                                          return e.first == owner;
                                      }),
                       entries_.end());
    };
    return c;
}

const WalletRegistry::Fetch* WalletRegistry::find(const std::string& owner) const {
    for (const auto& entry : entries_) {
        if (entry.first == owner)
            return &entry.second;
    }
    return nullptr;
}

std::vector<ExtensionItem> WalletRegistry::items() const {
    std::vector<ExtensionItem> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back({CapabilityKind::Wallet, entry.first, "wallet", "account balance"});
    }
    return out;
}

// ---------------------------------------------------------------------------
// AllowanceRegistry
// ---------------------------------------------------------------------------

Contribution AllowanceRegistry::add(const std::string& owner, Fetch fetch) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const std::pair<std::string, Fetch>& e) { return e.first == owner; }),
        entries_.end());
    entries_.emplace_back(owner, std::move(fetch));

    Contribution c;
    c.kind = CapabilityKind::Allowance;
    c.name = owner;
    c.remove = [this, owner] {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const std::pair<std::string, Fetch>& e) {
                                          return e.first == owner;
                                      }),
                       entries_.end());
    };
    return c;
}

const AllowanceRegistry::Fetch* AllowanceRegistry::find(const std::string& owner) const {
    for (const auto& entry : entries_) {
        if (entry.first == owner)
            return &entry.second;
    }
    return nullptr;
}

std::vector<ExtensionItem> AllowanceRegistry::items() const {
    std::vector<ExtensionItem> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back(
            {CapabilityKind::Allowance, entry.first, "allowance", "subscription/quota windows"});
    }
    return out;
}

// ---------------------------------------------------------------------------
// PanelRegistry
// ---------------------------------------------------------------------------

Contribution PanelRegistry::add(const std::string& owner, PanelSpec spec) {
    Entry entry;
    entry.owner = owner;
    entry.spec = std::move(spec);
    entry.seq = next_seq_++;
    const std::string id = entry.spec.id;
    entries_.push_back(std::move(entry));

    Contribution c;
    c.kind = CapabilityKind::Panel;
    c.name = id;
    c.remove = [this, owner, id] {
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [&](const Entry& e) { return e.owner == owner && e.spec.id == id; }),
            entries_.end());
    };
    return c;
}

const PanelSpec* PanelRegistry::find(const std::string& id) const {
    for (const auto& entry : entries_) {
        if (entry.spec.id == id)
            return &entry.spec;
    }
    return nullptr;
}

std::vector<PanelSpec> PanelRegistry::all() const {
    std::vector<PanelSpec> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_)
        out.push_back(entry.spec);
    return out;
}

std::vector<ExtensionItem> PanelRegistry::items() const {
    std::vector<ExtensionItem> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back({CapabilityKind::Panel, entry.owner, entry.spec.id, entry.spec.title});
    }
    return out;
}

// ---------------------------------------------------------------------------
// PluginServices
// ---------------------------------------------------------------------------

PluginServices::PluginServices(ToolRegistry& tools, PromptRegistry& prompts, StatusRegistry& status,
                               PanelRegistry& panels, WalletRegistry& wallets,
                               AllowanceRegistry& allowances, EventBus& events) noexcept
    : tools_(&tools), prompts_(&prompts), status_(&status), panels_(&panels), wallets_(&wallets),
      allowances_(&allowances), events_(&events) {}

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

namespace {

ToolMeta meta_for_tool(const ToolCapability::Meta& declared, const std::string& tool_name) {
    const auto it = declared.find(tool_name);
    return it == declared.end() ? ToolMeta{} : it->second;
}

} // namespace

ToolCapability::ToolCapability(std::string name, std::unique_ptr<Tool> tool, Meta meta)
    : name_(std::move(name)), meta_(std::move(meta)), tool_(std::move(tool)) {}

ToolCapability::ToolCapability(std::string name, Factory factory, Meta meta)
    : name_(std::move(name)), meta_(std::move(meta)), factory_(std::move(factory)) {}

InstallResult ToolCapability::install(PluginServices& services) {
    InstallResult r;
    // The factory form builds against the harness services; the direct form was
    // already built by the plugin.
    std::vector<std::unique_ptr<Tool>> tools;
    if (tool_) {
        tools.push_back(std::move(tool_));
    } else if (factory_) {
        tools = factory_(services);
    }
    if (tools.empty()) {
        // The factory returned nothing: the tool is gated off, not broken. The
        // plugin stays active with one fewer contribution.
        r.declined = true;
        return r;
    }

    ToolRegistry* registry = &services.tools();
    // Register under the contributing plugin's id. Ownership is what lets the
    // ledger unwind without reaching across plugins: two of them may contribute
    // the same tool name (the later registration wins), and the one that lost
    // the name must not remove the winner's tool on its way out.
    const std::string owner = services.owner();
    std::vector<std::string> registered;
    registered.reserve(tools.size());
    for (auto& tool : tools) {
        if (!tool)
            continue;
        registered.push_back(tool->name());
        // The meta travels with the registration, so the UI reads the verb the
        // plugin declared instead of keeping its own name→verb table.
        registry->register_tool(std::move(tool), owner,
                                    meta_for_tool(meta_, registered.back()));
    }
    if (registered.empty()) {
        r.declined = true;
        return r;
    }

    r.ok = true;
    r.contribution.kind = CapabilityKind::Tool;
    r.contribution.name = registered.front();
    // Every tool this capability installed goes away together, and only these:
    // the ledger records one contribution, so its removal must undo all of them
    // and nothing else. Removal is owner-checked, so a name that was taken over
    // by another plugin stays with its new owner.
    r.contribution.remove = [registry, registered, owner] {
        for (const auto& name : registered)
            registry->remove_owned_tool(name, owner);
    };
    return r;
}

PromptBlockCapability::PromptBlockCapability(std::string id, int priority,
                                             PromptRegistry::Render render,
                                             PromptPlacement placement)
    : id_(std::move(id)), priority_(priority), render_(std::move(render)), placement_(placement) {}

InstallResult PromptBlockCapability::install(PluginServices& services) {
    InstallResult r;
    if (!render_) {
        r.error = "prompt block '" + id_ + "' has no renderer";
        return r;
    }
    r.contribution = services.prompts().add(services.owner(), id_, priority_, render_, placement_);
    r.ok = true;
    return r;
}

ProviderCapability::ProviderCapability(std::string flavor,
                                       std::function<std::unique_ptr<class Dialect>()> make_dialect,
                                       std::vector<Preset> presets)
    : flavor_(std::move(flavor)), make_dialect_(std::move(make_dialect)),
      presets_(std::move(presets)) {}

// Out-of-line so the header needs only a forward declaration of Dialect.
ProviderCapability::~ProviderCapability() = default;

InstallResult ProviderCapability::install(PluginServices& services) {
    InstallResult r;
    if (flavor_.empty()) {
        r.error = "provider capability needs a flavor";
        return r;
    }
    const std::string owner = services.owner();
    // A factory means this plugin *provides* the protocol; without one it only
    // speaks a protocol someone else provides (the shared openai dialect).
    const bool provides_dialect = static_cast<bool>(make_dialect_);
    if (provides_dialect)
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
    r.contribution.remove = [owner, flavor = flavor_, provides_dialect] {
        // Take back exactly this plugin's contribution: the dialect it
        // provided (marked unavailable, so a provider file pointing at it fails
        // loudly rather than speaking another protocol) and its preset rows.
        // A presets-only provider leaves the shared dialect alone.
        if (provides_dialect)
            unregister_dialect(flavor, owner);
        unregister_provider_presets_for(owner);
    };
    return r;
}

StatusSegmentCapability::StatusSegmentCapability(std::string id, int priority, int drop_priority,
                                                 StatusRegistry::Render render)
    : id_(std::move(id)), priority_(priority), drop_priority_(drop_priority),
      render_(std::move(render)) {}

InstallResult StatusSegmentCapability::install(PluginServices& services) {
    InstallResult r;
    if (id_.empty() || !render_) {
        r.error = "status segment capability needs an id and a renderer";
        return r;
    }
    r.contribution =
        services.status().add(services.owner(), id_, priority_, drop_priority_, render_);
    r.ok = true;
    return r;
}

WalletCapability::WalletCapability(WalletRegistry::Fetch fetch) : fetch_(std::move(fetch)) {}

InstallResult WalletCapability::install(PluginServices& services) {
    InstallResult r;
    if (!fetch_) {
        r.error = "wallet capability needs a fetch";
        return r;
    }
    const std::string owner = services.owner();
    r.contribution = services.wallets().add(owner, std::move(fetch_));
    r.ok = true;
    return r;
}

AllowanceCapability::AllowanceCapability(AllowanceRegistry::Fetch fetch)
    : fetch_(std::move(fetch)) {}

InstallResult AllowanceCapability::install(PluginServices& services) {
    InstallResult r;
    if (!fetch_) {
        r.error = "allowance capability needs a fetch";
        return r;
    }
    const std::string owner = services.owner();
    r.contribution = services.allowances().add(owner, std::move(fetch_));
    r.ok = true;
    return r;
}

PanelCapability::PanelCapability(PanelSpec spec) : spec_(std::move(spec)) {}

InstallResult PanelCapability::install(PluginServices& services) {
    InstallResult r;
    if (spec_.id.empty() || !spec_.lines) {
        r.error = "panel capability needs an id and a renderer";
        return r;
    }
    r.contribution = services.panels().add(services.owner(), std::move(spec_));
    r.ok = true;
    return r;
}

} // namespace agent

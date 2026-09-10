#ifndef AGENT_PLUGIN_CAPABILITY_H
#define AGENT_PLUGIN_CAPABILITY_H

// Plugin capability protocol and the contribution ledger (spec §3).
//
// A plugin *declares* typed capabilities; the runtime installs them and records
// each install in the ledger. Disable unwinds the ledger in reverse order, so
// nothing a plugin contributed can survive deactivation. A plugin that installs
// outside this path cannot be disabled cleanly — that is why the protocol
// exists.
//
// Payloads are typed: there is no void* in the plugin-facing API.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace agent {

// The surface a capability installs into. Extension registries attach here as
// they land (PF-1.4); a capability that only reports what it contributes does
// not touch it.
class PluginServices {
public:
    virtual ~PluginServices() = default;
};

enum class CapabilityKind : std::uint8_t {
    Tool,
    Command,
    PromptBlock,
    Setting,
    Provider,
};

// A live contribution. The owner is the plugin id; `remove` unwinds it and is
// idempotent. A null `remove` means the contribution has nothing to unwind.
struct Contribution {
    CapabilityKind kind = CapabilityKind::Tool;
    std::string name;
    std::function<void()> remove;
};

struct InstallResult {
    bool ok = false;
    std::string error;         // non-empty when ok == false
    Contribution contribution;
};

class Capability {
public:
    virtual ~Capability() = default;

    // Unique within the plugin.
    virtual std::string name() const = 0;
    virtual CapabilityKind kind() const = 0;

    // Install into the harness. Returns the handle the ledger records; on
    // failure the harness installs nothing, so a failed install must leave no
    // partial state behind.
    virtual InstallResult install(PluginServices& services) = 0;
};

// Per-plugin record of installed contributions, unwound in reverse install
// order (last installed, first removed).
class PluginLedger {
public:
    void record(const std::string& plugin_id, Contribution c);

    std::size_t size(const std::string& plugin_id) const noexcept;
    std::vector<Contribution> contributions(const std::string& plugin_id) const;
    std::vector<std::string> owners() const;

    // Remove everything the plugin contributed, newest first. Safe to call for
    // an unknown plugin. A throwing remove handler never aborts the unwind.
    void unwind(const std::string& plugin_id) noexcept;

    void clear() noexcept;

private:
    std::vector<std::pair<std::string, Contribution>> entries_;
};

} // namespace agent

#endif // AGENT_PLUGIN_CAPABILITY_H

#include "agent/dialect.h"
#include "agent/dialect_openai.h"

#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace agent {

namespace {

using DialectFactory = std::function<std::unique_ptr<Dialect>()>;

struct DialectEntry {
    DialectFactory factory;
    std::string owner;  // plugin id, empty for built-ins
};

// The table is mutated when a provider plugin is toggled at runtime and read
// on every client construction, so it carries its own lock: resolution must
// never race registration (spec invariant 12).
std::mutex& dialect_mutex() {
    static std::mutex mtx;
    return mtx;
}

std::map<std::string, DialectEntry>& dialect_table() {
    // Only the transport's own protocol is built in. Every vendor protocol is
    // registered by the provider plugin that owns it (anthropic by the
    // anthropic plugin, gemini by the gemini plugin, ...), so switching a
    // provider off takes its protocol out of the table with it.
    static std::map<std::string, DialectEntry> table = {
        {"openai", {[]() { return make_openai_dialect(); }, ""}},
    };
    return table;
}

// Flavors a plugin used to provide. Kept separate from the table so an
// unknown flavor still falls back while a disabled one refuses.
std::map<std::string, std::string>& unavailable_flavors() {
    static std::map<std::string, std::string> flavors;  // flavor -> plugin id
    return flavors;
}

std::unique_ptr<Dialect> find_locked(const std::string& flavor) {
    auto it = dialect_table().find(flavor);
    if (it != dialect_table().end()) return it->second.factory();
    auto fallback = dialect_table().find("openai");
    return fallback->second.factory();
}

} // namespace

std::unique_ptr<Dialect> make_dialect(const std::string& flavor) {
    std::scoped_lock lock(dialect_mutex());
    return find_locked(flavor);
}

void register_dialect(const std::string& flavor,
                      std::function<std::unique_ptr<Dialect>()> factory,
                      const std::string& owner) {
    std::scoped_lock lock(dialect_mutex());
    dialect_table()[flavor] = DialectEntry{std::move(factory), owner};
    // Re-registering (a plugin re-enabled) clears the unavailable mark.
    auto it = unavailable_flavors().find(flavor);
    if (it != unavailable_flavors().end() && it->second == owner)
        unavailable_flavors().erase(it);
}

void unregister_dialects_for(const std::string& owner) {
    if (owner.empty()) return;
    std::scoped_lock lock(dialect_mutex());
    for (auto it = dialect_table().begin(); it != dialect_table().end();) {
        if (it->second.owner == owner) {
            unavailable_flavors()[it->first] = owner;
            it = dialect_table().erase(it);
        } else {
            ++it;
        }
    }
}

bool unregister_dialect(const std::string& flavor, const std::string& owner) {
    if (owner.empty()) return false;
    std::scoped_lock lock(dialect_mutex());
    auto it = dialect_table().find(flavor);
    if (it == dialect_table().end() || it->second.owner != owner) return false;
    unavailable_flavors()[flavor] = owner;
    dialect_table().erase(it);
    return true;
}

void declare_flavor(const std::string& flavor, const std::string& owner) {
    if (flavor.empty() || owner.empty()) return;
    std::scoped_lock lock(dialect_mutex());
    // A protocol someone is providing right now needs no declaration: a
    // presets-only provider "declaring" the shared openai dialect must never
    // make that dialect look unavailable.
    if (dialect_table().count(flavor)) return;
    unavailable_flavors()[flavor] = owner;
}

std::string flavor_unavailable_reason(const std::string& flavor) {
    std::scoped_lock lock(dialect_mutex());
    if (dialect_table().count(flavor)) return {};   // currently provided
    auto it = unavailable_flavors().find(flavor);
    if (it == unavailable_flavors().end()) return {};  // nobody provides it: a typo
    return "provider flavor '" + flavor + "' is provided by plugin '" +
           it->second + "', which is disabled - enable it with /set plugin " +
           it->second + " on";
}

} // namespace agent

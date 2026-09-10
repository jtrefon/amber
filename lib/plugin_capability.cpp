#include "agent/plugin_capability.h"

#include <algorithm>

namespace agent {

void PluginLedger::record(const std::string& plugin_id, Contribution c) {
    entries_.emplace_back(plugin_id, std::move(c));
}

std::size_t PluginLedger::size(const std::string& plugin_id) const noexcept {
    std::size_t count = 0;
    for (const auto& entry : entries_) {
        if (entry.first == plugin_id) ++count;
    }
    return count;
}

std::vector<Contribution>
PluginLedger::contributions(const std::string& plugin_id) const {
    std::vector<Contribution> out;
    for (const auto& entry : entries_) {
        if (entry.first == plugin_id) out.push_back(entry.second);
    }
    return out;
}

std::vector<std::string> PluginLedger::owners() const {
    std::vector<std::string> out;
    for (const auto& entry : entries_) {
        if (std::find(out.begin(), out.end(), entry.first) == out.end())
            out.push_back(entry.first);
    }
    return out;
}

void PluginLedger::unwind(const std::string& plugin_id) noexcept {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->first != plugin_id) continue;
        if (it->second.remove) {
            // A plugin's teardown must not be able to abort the unwind: the
            // rest of its contributions still have to come out.
            try {
                it->second.remove();
            } catch (...) {
            }
        }
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const std::pair<std::string, Contribution>& e) {
                                      return e.first == plugin_id;
                                  }),
                   entries_.end());
}

void PluginLedger::clear() noexcept {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->second.remove) {
            try {
                it->second.remove();
            } catch (...) {
            }
        }
    }
    entries_.clear();
}

} // namespace agent

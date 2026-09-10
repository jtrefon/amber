#include "agent/plugin_console.h"

#include <algorithm>

namespace agent {

namespace {

const char* kind_name(CapabilityKind kind) {
    switch (kind) {
    case CapabilityKind::Tool: return "tool";
    case CapabilityKind::Command: return "command";
    case CapabilityKind::PromptBlock: return "prompt";
    case CapabilityKind::StatusSegment: return "segment";
    case CapabilityKind::Panel: return "panel";
    case CapabilityKind::Setting: return "setting";
    case CapabilityKind::Provider: return "provider";
    }
    return "?";
}

// Pad to `width` so the columns line up without a table renderer.
std::string pad(const std::string& s, std::size_t width) {
    if (s.size() >= width) return s + " ";
    return s + std::string(width - s.size(), ' ') + " ";
}

} // namespace

std::vector<std::string> plugin_console_lines(const PluginRuntime& runtime) {
    const auto plugins = runtime.list();
    std::size_t on = 0;
    for (const auto& p : plugins)
        if (p.enabled) ++on;

    std::vector<std::string> lines;
    lines.push_back("plugins: " + std::to_string(plugins.size()) +
                    " registered, " + std::to_string(on) + " on");
    lines.push_back("");
    lines.push_back("  " + pad("id", 16) + pad("tier", 10) + pad("state", 7) +
                    "contributions");

    for (const auto& p : plugins) {
        std::string line = "  " + pad(p.id, 16) + pad(p.tier, 10) +
                           pad(p.enabled ? "on" : "off", 7);
        if (p.version.empty()) {
            line += "v?";
        } else {
            line += "v" + p.version;
        }
        if (p.contributions.empty()) {
            line += "  (observes only)";
        } else {
            line += "  [";
            for (std::size_t i = 0; i < p.contributions.size(); ++i) {
                if (i) line += ", ";
                line += std::string(kind_name(p.contributions[i].kind)) + ":" +
                        p.contributions[i].name;
            }
            line += "]";
        }
        lines.push_back(std::move(line));
    }

    const auto panels = runtime.panels().items();
    lines.push_back("");
    lines.push_back("panels: " + std::to_string(panels.size()));
    for (const auto& panel : panels) {
        const std::string owner = panel.owner.empty() ? "core" : panel.owner;
        lines.push_back("  " + pad(panel.name, 16) + pad(owner, 16) +
                        panel.detail);
    }

    lines.push_back("");
    lines.push_back("toggle a plugin with /set plugin <id> on|off");
    return lines;
}

void register_console_panel(PanelRegistry& panels,
                            const PluginRuntime& runtime) {
    PanelSpec spec;
    spec.id = "plugins";
    spec.title = "Plugin registry";
    // Reads the runtime on every repaint, so the view is never stale: a
    // toggle elsewhere shows up the next time it is drawn.
    spec.lines = [&runtime](int) { return plugin_console_lines(runtime); };
    panels.add("", std::move(spec));
}

} // namespace agent

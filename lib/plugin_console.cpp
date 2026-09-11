#include "agent/plugin_console.h"

#include <algorithm>

namespace agent {

const char* capability_kind_name(CapabilityKind kind) {
    switch (kind) {
    case CapabilityKind::Tool:
        return "tool";
    case CapabilityKind::Command:
        return "command";
    case CapabilityKind::PromptBlock:
        return "prompt";
    case CapabilityKind::StatusSegment:
        return "segment";
    case CapabilityKind::Panel:
        return "panel";
    case CapabilityKind::Setting:
        return "setting";
    case CapabilityKind::Provider:
        return "provider";
    case CapabilityKind::Wallet:
        return "wallet";
    }
    return "?";
}

namespace {

// Pad to `width` so the columns line up without a table renderer.
std::string pad(const std::string& s, std::size_t width) {
    if (s.size() >= width)
        return s + " ";
    return s + std::string(width - s.size(), ' ') + " ";
}

} // namespace

namespace {

// Order the known categories sensibly (the ones a user looks for first, first);
// anything else follows alphabetically, and "other" always comes last. A plugin
// is free to invent a category — an unknown one still gets its own group rather
// than being hidden or folded into "other".
int category_rank(const std::string& category) {
    static const char* kOrder[] = {
        plugin_category::kProvider, plugin_category::kTools,  plugin_category::kObservability,
        plugin_category::kUi,       plugin_category::kMemory, plugin_category::kSearch,
    };
    for (std::size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); ++i)
        if (category == kOrder[i])
            return static_cast<int>(i);
    if (category == plugin_category::kOther)
        return 1000;
    return 500; // unknown: after the known set, before "other"
}

} // namespace

std::vector<std::string> plugin_console_lines(const PluginRuntime& runtime) {
    const auto plugins = runtime.list();
    std::size_t on = 0;
    for (const auto& p : plugins)
        if (p.enabled)
            ++on;

    std::vector<std::string> lines;
    lines.push_back("plugins: " + std::to_string(plugins.size()) + " registered, " +
                    std::to_string(on) + " on");

    // One group per category, so a long list is scanned by heading rather than
    // read line by line.
    std::vector<std::string> categories;
    for (const auto& p : plugins) {
        if (std::find(categories.begin(), categories.end(), p.category) == categories.end())
            categories.push_back(p.category);
    }
    std::sort(categories.begin(), categories.end(), [](const std::string& a, const std::string& b) {
        const int ra = category_rank(a), rb = category_rank(b);
        return ra != rb ? ra < rb : a < b;
    });

    for (const auto& category : categories) {
        lines.push_back("");
        lines.push_back(category);
        for (const auto& p : plugins) {
            if (p.category != category)
                continue;
            std::string line = "  " + pad(p.id, 14) + pad(p.enabled ? "on" : "off", 4) +
                               pad(p.version.empty() ? "v?" : "v" + p.version, 9);
            // The description answers "which one do I want"; the tier and the
            // exact contributions are one command away (/get plugin <id>).
            line += p.description.empty() ? "(no description)" : p.description;
            lines.push_back(std::move(line));

            if (p.contributions.empty()) {
                lines.push_back("      observes only");
                continue;
            }
            std::string detail = "      ";
            for (std::size_t i = 0; i < p.contributions.size(); ++i) {
                if (i)
                    detail += ", ";
                detail += std::string(capability_kind_name(p.contributions[i].kind)) + ":" +
                          p.contributions[i].name;
            }
            lines.push_back(std::move(detail));
        }
    }

    const auto panels = runtime.panels().items();
    lines.push_back("");
    lines.push_back("panels: " + std::to_string(panels.size()));
    for (const auto& panel : panels) {
        const std::string owner = panel.owner.empty() ? "core" : panel.owner;
        lines.push_back("  " + pad(panel.name, 16) + pad(owner, 16) + panel.detail);
    }

    lines.push_back("");
    lines.push_back("toggle a plugin with /set plugin <id> on|off");
    return lines;
}

void register_console_panel(PanelRegistry& panels, const PluginRuntime& runtime) {
    PanelSpec spec;
    spec.id = "plugins";
    spec.title = "Plugin registry";
    // Reads the runtime on every repaint, so the view is never stale: a
    // toggle elsewhere shows up the next time it is drawn.
    spec.lines = [&runtime](int) { return plugin_console_lines(runtime); };
    panels.add("", std::move(spec));
}

} // namespace agent

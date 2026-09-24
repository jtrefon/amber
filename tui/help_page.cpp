#include "tui/help_page.h"

namespace tui::help_page {

std::string key_from_node(const std::string& node) {
    std::string n = node;
    if (!n.empty() && n[0] == '/')
        n = n.substr(1);
    const size_t sp = n.find(' ');
    return sp == std::string::npos ? n : n.substr(sp + 1);
}

std::string command_from_node(const std::string& node) {
    std::string n = node;
    if (!n.empty() && n[0] == '/')
        n = n.substr(1);
    const size_t sp = n.find(' ');
    if (sp != std::string::npos)
        n.resize(sp);
    return n;
}

std::vector<std::string> build(const SettingRegistry& settings, const std::string& key) {
    const std::string man = settings.man_for(key);
    if (man.empty())
        return {};

    std::vector<std::string> page;
    // Header: the help text as a subtitle.
    const std::string helptxt = settings.help_for(key);
    if (!helptxt.empty())
        page.emplace_back(helptxt);
    page.emplace_back("");
    // Body: the full man text, one row per source line.
    size_t pos = 0;
    while (pos < man.size()) {
        const size_t next = man.find('\n', pos);
        if (next == std::string::npos) {
            page.emplace_back(man.substr(pos));
            break;
        }
        page.emplace_back(man.substr(pos, next - pos));
        pos = next + 1;
    }
    page.emplace_back("");
    // Children listing, each with its own help text when present.
    const std::vector<std::string> kids = settings.children_of(key);
    if (!kids.empty()) {
        page.emplace_back("sub-commands:");
        for (const auto& k : kids) {
            std::string line = "  " + k;
            std::string subkey = key;
            subkey += '.';
            subkey += k;
            const std::string h = settings.help_for(subkey);
            if (!h.empty()) {
                line += "  —  ";
                line += h;
            }
            page.emplace_back(line);
        }
        page.emplace_back("");
    }
    // Choices / range trailer for leaf settings.
    const std::vector<std::string>& choices = settings.choices_for(key);
    if (!choices.empty()) {
        std::string line = "choices: ";
        for (size_t i = 0; i < choices.size(); ++i) {
            if (i > 0)
                line += ", ";
            line += choices[i];
        }
        page.push_back(line);
    }
    double lo = 0, hi = 0;
    if (settings.range_for(key, lo, hi)) {
        std::string line = "range: ";
        line += std::to_string(static_cast<int>(lo));
        line += " – ";
        line += std::to_string(static_cast<int>(hi));
        page.push_back(std::move(line));
    }
    return page;
}

std::string fallback_line(const SettingRegistry& settings, const std::string& key) {
    const std::string desc = settings.help_for(key);
    if (desc.empty())
        return "";
    std::string msg = key;
    msg += "  —  ";
    msg += desc;
    const std::vector<std::string>& choices = settings.choices_for(key);
    if (!choices.empty()) {
        msg += "  choices: ";
        for (const auto& c : choices)
            msg += c + "|";
        msg.pop_back();
    }
    double lo = 0, hi = 0;
    if (settings.range_for(key, lo, hi)) {
        msg += "  range: ";
        msg += std::to_string(lo);
        msg += '-';
        msg += std::to_string(hi);
    }
    return msg;
}

} // namespace tui::help_page

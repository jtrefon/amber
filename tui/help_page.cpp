#include "tui/help_page.h"

#include <optional>

namespace tui::help_page {

namespace {

// The man text, one row per source line.
std::vector<std::string> split_man_lines(const std::string& man) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < man.size()) {
        const size_t next = man.find('\n', pos);
        if (next == std::string::npos) {
            out.emplace_back(man.substr(pos));
            break;
        }
        out.emplace_back(man.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

// Each sub-command with its own help text when present.
std::vector<std::string> sub_command_lines(const SettingRegistry& settings, const std::string& key,
                                           const std::vector<std::string>& kids) {
    std::vector<std::string> out;
    out.emplace_back("sub-commands:");
    for (const auto& k : kids) {
        std::string line = "  " + k;
        const std::string h = settings.help_for(key + "." + k);
        if (!h.empty())
            line += "  —  " + h;
        out.emplace_back(line);
    }
    out.emplace_back("");
    return out;
}

std::optional<std::string> choices_line(const std::vector<std::string>& choices) {
    if (choices.empty())
        return std::nullopt;
    std::string line = "choices: ";
    for (size_t i = 0; i < choices.size(); ++i) {
        if (i > 0)
            line += ", ";
        line += choices[i];
    }
    return line;
}

std::optional<std::string> range_line(const SettingRegistry& settings, const std::string& key) {
    double lo = 0, hi = 0;
    if (!settings.range_for(key, lo, hi))
        return std::nullopt;
    return "range: " + std::to_string(static_cast<int>(lo)) + " – " +
           std::to_string(static_cast<int>(hi));
}

} // namespace

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
    const std::vector<std::string> body = split_man_lines(man);
    page.insert(page.end(), body.begin(), body.end());
    page.emplace_back("");
    // Children listing, each with its own help text when present.
    const std::vector<std::string> kids = settings.children_of(key);
    if (!kids.empty()) {
        const std::vector<std::string> sub = sub_command_lines(settings, key, kids);
        page.insert(page.end(), sub.begin(), sub.end());
    }
    // Choices / range trailer for leaf settings.
    if (const auto line = choices_line(settings.choices_for(key)))
        page.push_back(*line);
    if (const auto line = range_line(settings, key))
        page.push_back(*line);
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

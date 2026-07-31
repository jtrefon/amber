
#include "palette.h"

#include <sstream>

namespace tui::palette {

// =========================================================================
// Legacy flat Command helpers
// =========================================================================

std::string token(const std::string& input) {
    if (input.empty() || input[0] != '/') return "";
    std::string rest = input.substr(1);
    size_t sp = rest.find(' ');
    return sp == std::string::npos ? rest : rest.substr(0, sp);
}

bool has_arg(const std::string& input) {
    return input.find(' ') != std::string::npos;
}

bool wants_open(const std::string& input) {
    return !input.empty() && input[0] == '/';
}

std::vector<const Command*> filter(const std::vector<Command>& commands,
                                   const std::string& tok) {
    std::vector<const Command*> exact, prefix, aliased;
    for (const auto& c : commands) {
        if (tok.empty() || c.name == tok) {
            exact.push_back(&c);
        } else if (c.name.rfind(tok, 0) == 0) {
            prefix.push_back(&c);
        } else {
            for (const auto& a : c.aliases)
                if (a == tok) { exact.push_back(&c); break; }
                else if (a.rfind(tok, 0) == 0) { aliased.push_back(&c); break; }
        }
    }
    exact.insert(exact.end(), prefix.begin(), prefix.end());
    exact.insert(exact.end(), aliased.begin(), aliased.end());
    return exact;
}

const Command* find(const std::vector<Command>& commands,
                    const std::string& name) {
    for (const auto& c : commands) {
        if (c.name == name) return &c;
        for (const auto& a : c.aliases)
            if (a == name) return &c;
    }
    return nullptr;
}

std::string common_prefix(const std::vector<std::string>& names) {
    if (names.empty()) return "";
    std::string p = names.front();
    for (const auto& n : names) {
        size_t i = 0;
        while (i < p.size() && i < n.size() && p[i] == n[i]) ++i;
        p.resize(i);
    }
    return p;
}

std::string usage(const Command& c) {
    std::string u = "/" + c.name;
    if (!c.args.empty()) u += " " + c.args;
    return u;
}

}  // namespace tui::palette

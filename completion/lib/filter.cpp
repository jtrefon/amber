// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "completion/filter.h"

namespace completion {

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

std::vector<const Command*> filter_top(
    const std::vector<std::unique_ptr<Command>>& commands,
    const std::string& tok) {
    std::vector<const Command*> primary, aliased;
    for (const auto& c : commands) {
        if (tok.empty() || c->name.rfind(tok, 0) == 0) {
            primary.push_back(c.get());
        } else {
            for (const auto& a : c->aliases)
                if (a.rfind(tok, 0) == 0) { aliased.push_back(c.get()); break; }
        }
    }
    primary.insert(primary.end(), aliased.begin(), aliased.end());
    return primary;
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

ParsedInput parse_input(const std::string& input) {
    ParsedInput r;
    if (input.empty() || input[0] != '/') return r;
    std::string rest = input.substr(1);
    if (rest.empty()) return r;
    size_t pos = 0;
    while (pos < rest.size()) {
        size_t sp = rest.find(' ', pos);
        if (sp == std::string::npos) {
            r.partial = rest.substr(pos);
            break;
        }
        std::string tok = rest.substr(pos, sp - pos);
        if (!tok.empty()) r.tokens.push_back(tok);
        pos = sp + 1;
        if (pos >= rest.size()) {
            r.ends_with_space = true;
            break;
        }
    }
    return r;
}

std::string usage_line(const Command& cmd) {
    std::string u = "/" + cmd.name;
    if (!cmd.args_usage.empty()) u += " " + cmd.args_usage;
    return u;
}

} // namespace completion

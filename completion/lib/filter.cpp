// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "completion/filter.h"

#include <algorithm>

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

TopCompletion top_completion(const std::vector<std::unique_ptr<Command>>& commands,
                              const std::string& input) {
    TopCompletion r;
    r.expanded = input;

    if (input.empty() || input[0] != '/') return r;

    auto pi = parse_input(input);

    // Top-level completion: extend the command name.
    if (pi.tokens.empty()) {
        std::string tok = token(input);
        if (tok.empty()) return r;
        auto matches = filter_top(commands, tok);
        if (matches.empty()) return r;

        // If only one match, complete to it.
        if (matches.size() == 1) {
            r.expanded = "/" + matches[0]->name;
            if (tok.size() < matches[0]->name.size())
                r.shadow = matches[0]->name.substr(tok.size());
            return r;
        }

        // Multiple matches: extend to common prefix.
        std::vector<std::string> names;
        for (auto* c : matches) names.push_back(c->name);
        std::string cp = common_prefix(names);
        if (cp.size() > tok.size()) {
            r.expanded = "/" + cp;
            r.shadow = cp.substr(tok.size());
        }
        return r;
    }

    // Argument-level completion: walk the command tree.
    const Command* current = nullptr;
    for (size_t i = 0; i < pi.tokens.size(); ++i) {
        if (i == 0) {
            for (const auto& c : commands) {
                if (c->name == pi.tokens[i]) { current = c.get(); break; }
                for (const auto& a : c->aliases)
                    if (a == pi.tokens[i]) { current = c.get(); break; }
            }
        } else if (current) {
            current = current->find_subcommand(pi.tokens[i]);
        }
        if (!current) return r;
    }

    // We're at the current command, partial is the next word.
    std::string partial = pi.partial;
    // Collect subcommand names.
    std::vector<std::string> choices;
    for (const auto& sc : current->subcommands) {
        if (partial.empty() || sc->name.rfind(partial, 0) == 0 ||
            std::any_of(sc->aliases.begin(), sc->aliases.end(),
                [&](const std::string& a) { return a.rfind(partial, 0) == 0; }))
            choices.push_back(sc->name);
    }
    if (choices.empty()) return r;

    if (choices.size() == 1) {
        std::string tail = choices[0];
        const Command* matched = current->find_subcommand(tail);
        bool has_more = matched && !matched->subcommands.empty();
        r.expanded = "/" + pi.tokens[0] + " " + tail;
        if (!has_more) r.expanded += " ";
        if (partial.size() < tail.size())
            r.shadow = tail.substr(partial.size());
        return r;
    }

    std::string cp = common_prefix(choices);
    if (cp.size() > partial.size()) {
        std::string tail = pi.tokens[0] + " " + cp;
        r.expanded = "/" + tail;
        r.shadow = cp.substr(partial.size());
    }
    return r;
}

} // namespace completion

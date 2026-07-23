// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "palette.h"

namespace tui::palette {

// =========================================================================
// Free helpers
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
    std::vector<const Command*> primary, aliased;
    for (const auto& c : commands) {
        if (tok.empty() || c.name.rfind(tok, 0) == 0) {
            primary.push_back(&c);
        } else {
            for (const auto& a : c.aliases)
                if (a.rfind(tok, 0) == 0) { aliased.push_back(&c); break; }
        }
    }
    primary.insert(primary.end(), aliased.begin(), aliased.end());
    return primary;
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

// =========================================================================
// Completer
// =========================================================================

TabResult Completer::handle_tab(const std::vector<Command>& commands,
                                 const std::string& input,
                                 int drawer_sel) {
    // Argument mode: input has a space after the command name.
    if (has_arg(input)) {
        size_t sp = input.find(' ');
        std::string name = input.substr(1, sp - 1);
        const Command* c = find(commands, name);
        if (c && c->complete_arg) {
            std::string partial = input.substr(sp + 1);
            TabResult r = complete_arg(*c, partial);
            if (!r.input.empty()) {
                // On a new Tab sequence (input changed since last Tab), reset.
                if (input != last_tab_input_)
                    consecutive_tabs_ = 0;
                ++consecutive_tabs_;
                last_tab_input_ = r.input;
                r.close_drawer = true;
                return r;
            }
        }
    }

    // Command-name completion via the drawer.
    return complete_cmd_name(commands, input, drawer_sel);
}

std::vector<const Command*> Completer::drawer_matches(
    const std::vector<Command>& commands,
    const std::string& input) const {
    std::string tok = token(input);
    return filter(commands, tok);
}

TabResult Completer::complete_cmd_name(const std::vector<Command>& commands,
                                        const std::string& input,
                                        int drawer_sel) {
    TabResult r;
    std::string tok = token(input);
    auto matches = filter(commands, tok);
    if (matches.empty()) { r.input = input; return r; }

    // Explicit selection from the drawer: complete to that command.
    if (drawer_sel >= 0 && drawer_sel < static_cast<int>(matches.size())) {
        r.input = "/" + matches[drawer_sel]->name + " ";
        return r;
    }

    // No explicit selection: extend to common prefix of matching names.
    std::vector<std::string> names;
    names.reserve(matches.size());
    for (auto* c : matches) names.push_back(c->name);
    std::string cp = common_prefix(names);
    if (cp.size() > tok.size()) {
        r.input = "/" + cp;                // extend to shared prefix
    } else if (matches.size() == 1) {
        r.input = "/" + matches[0]->name + " ";  // exact match: add space
    } else {
        r.input = input;                   // ambiguous, no extension
    }
    return r;
}

TabResult Completer::complete_arg(const Command& cmd,
                                   const std::string& partial) {
    TabResult r;
    auto choices = cmd.complete_arg(partial);
    if (choices.empty()) return r;

    // Single unambiguous match — complete immediately.
    if (choices.size() == 1) {
        r.input = "/" + cmd.name + " " + choices[0] + " ";
        return r;
    }

    // Multiple matches: compute shared prefix.
    std::string cp = common_prefix(choices);

    // If the prefix extends what the user typed, extend inline.
    if (cp.size() > partial.size()) {
        r.input = "/" + cmd.name + " " + cp;
        return r;
    }

    // Prefix doesn't extend: on the FIRST consecutive Tab at this point,
    // arm for a popup. On the SECOND consecutive Tab, show the popup.
    if (consecutive_tabs_ >= 1 && partial == cp) {
        r.show_popup = true;
        r.popup_items = choices;
        r.input = "/" + cmd.name + " " + partial;
        return r;
    }

    // First Tab at the prefix limit — arm for next Tab.
    r.input = "/" + cmd.name + " " + cp;
    return r;
}

}  // namespace tui::palette

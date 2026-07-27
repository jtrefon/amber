// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "completion/completer.h"
#include "completion/filter.h"

#include <algorithm>
#include <sstream>

namespace completion {

Completer::Context Completer::resolve_context(
    const std::vector<std::unique_ptr<Command>>& commands,
    const std::string& input) const {
    Context ctx;
    auto pi = parse_input(input);
    if (pi.tokens.empty()) return ctx;

    ctx.command = nullptr;
    // Walk the command tree following the tokens.
    const Command* current = nullptr;
    for (size_t i = 0; i < pi.tokens.size(); ++i) {
        const std::string& tok = pi.tokens[i];
        if (i == 0) {
            // Top-level command lookup.
            for (const auto& c : commands) {
                if (c->name == tok) { current = c.get(); break; }
                for (const auto& a : c->aliases)
                    if (a == tok) { current = c.get(); break; }
            }
        } else if (current) {
            // Subcommand lookup.
            current = const_cast<Command*>(current->find_subcommand(tok));
        }
        if (!current) {
            // No match: this token and everything after is unparsed.
            ctx.unparsed.push_back(tok);
            for (size_t j = i + 1; j < pi.tokens.size(); ++j)
                ctx.unparsed.push_back(pi.tokens[j]);
            break;
        }
    }

    ctx.command = current;

    // Build the full prefix string.
    if (current) {
        ctx.full_prefix = current->name;
        const Command* p = current->parent;
        while (p) {
            ctx.full_prefix = p->name + " " + ctx.full_prefix;
            p = p->parent;
        }
    } else if (!pi.tokens.empty()) {
        ctx.full_prefix = pi.tokens[0];
    }

    ctx.partial = pi.partial;
    return ctx;
}

CompletionResult Completer::complete_top_level(
    const std::vector<std::unique_ptr<Command>>& commands,
    const std::string& input, bool question_mark, const Context& /*ctx*/) {
    CompletionResult r;

    // Token from the input (everything after /, before space).
    std::string tok;
    {
        auto pi = parse_input(input);
        if (!pi.tokens.empty()) tok = pi.tokens[0];
        else tok = pi.partial;
    }

    auto matches = filter_top(commands, tok);
    if (matches.empty()) { r.new_input = input; return r; }

    if (question_mark) {
        // Cisco ?-style help: show all matches with descriptions.
        r.help_lines = help_for_commands(matches);
        r.new_input = input;
        return r;
    }

    // No explicit selection: extend to common prefix of matching names.
    std::vector<std::string> names;
    names.reserve(matches.size());
    for (auto* c : matches) names.push_back(c->name);
    std::string cp = common_prefix(names);
    if (cp.size() > tok.size()) {
        r.new_input = "/" + cp;
        r.shadow = cp.substr(tok.size());
    } else if (matches.size() == 1) {
        r.new_input = "/" + matches[0]->name + " ";
        r.close_drawer = true;
    } else {
        r.new_input = input;
        r.shadow = "";
    }
    return r;
}

CompletionResult Completer::complete_arg_level(
    const std::vector<std::unique_ptr<Command>>& /*commands*/,
    const std::string& input, bool question_mark, const Context& ctx) const {
    CompletionResult r;

    if (!ctx.command) return r;

    std::string partial = ctx.partial;
    const Command* cmd = ctx.command;

    // Build choices from args and subcommands.
    std::vector<std::string> choices;
    std::vector<std::string> choice_descriptions;
    std::vector<std::string> subcmd_help;

    // Subcommand names.
    for (const auto& sc : cmd->subcommands) {
        if (partial.empty() || sc->name.rfind(partial, 0) == 0 ||
            std::any_of(sc->aliases.begin(), sc->aliases.end(),
                [&](const std::string& a) { return a.rfind(partial, 0) == 0; })) {
            choices.push_back(sc->name);
            std::string desc = sc->description.empty() ? sc->name : sc->description;
            choice_descriptions.push_back("  " + sc->name + "  -  " + desc);
        }
    }

    // Argument completions from ArgSpec::complete.
    if (!cmd->args.empty() && cmd->args[0].complete) {
        auto arg_choices = cmd->args[0].complete(partial);
        for (auto& ac : arg_choices) {
            if (std::find(choices.begin(), choices.end(), ac) == choices.end()) {
                choices.push_back(ac);
                choice_descriptions.push_back("  " + ac);
            }
        }
    }

    if (choices.empty()) return r;

    if (question_mark) {
        // Cisco ?-style: show all available choices with descriptions.
        r.help_lines = choice_descriptions;
        // Also show command description if available.
        if (!cmd->description.empty())
            r.help_lines.insert(r.help_lines.begin(),
                cmd->usage() + "  -  " + cmd->description);
        r.new_input = input;
        return r;
    }

    // Single match: complete immediately.
    if (choices.size() == 1) {
        std::string tail = ctx.full_prefix.empty()
            ? choices[0]
            : ctx.full_prefix + " " + choices[0];
        r.new_input = "/" + tail;
        r.close_drawer = true;
        // If the matched command has NO subcommands, add trailing space.
        const Command* matched = cmd->find_subcommand(choices[0]);
        if (!matched || matched->subcommands.empty())
            r.new_input += " ";
        return r;
    }

    // Multiple matches: compute shared prefix.
    std::string cp = common_prefix(choices);
    std::string tail_prefix = ctx.full_prefix.empty()
        ? cp
        : ctx.full_prefix + " " + cp;

    if (cp.size() > partial.size()) {
        r.new_input = "/" + tail_prefix;
        r.shadow = cp.substr(partial.size());
        return r;
    }

    if (consecutive_tabs_ >= 1 && partial == cp) {
        r.show_popup = true;
        r.popup_items = choice_descriptions;
        r.new_input = "/" + (ctx.full_prefix.empty() ? cp : ctx.full_prefix + " " + cp);
        return r;
    }

    // First Tab at the prefix limit — arm for next Tab.
    r.new_input = "/" + tail_prefix;
    return r;
}

CompletionResult Completer::complete(
    const std::vector<std::unique_ptr<Command>>& commands,
    const std::string& input, bool question_mark) {
    CompletionResult r;

    // Parse input to determine context.
    auto pi = parse_input(input);
    Context ctx = resolve_context(commands, input);

    // Process completion FIRST while consecutive_tabs_ reflects the previous
    // call (0 on first Tab, 1+ on subsequent Tabs). Then update state.
    if (ctx.command && (!pi.tokens.empty() || !pi.partial.empty())) {
        r = complete_arg_level(commands, input, question_mark, ctx);
        if (!r.new_input.empty()) goto done;
    }
    r = complete_top_level(commands, input, question_mark, ctx);

done:
    // Track consecutive invocations for multi-tab state machine.
    if (input != last_tab_input_)
        consecutive_tabs_ = 0;
    ++consecutive_tabs_;
    last_tab_input_ = input;
    return r;
}

std::vector<const Command*> Completer::drawer_matches(
    const std::vector<std::unique_ptr<Command>>& commands,
    const std::string& input) const {
    std::string tok = token(input);
    return filter_top(commands, tok);
}

std::vector<std::string> Completer::help_for_commands(
    const std::vector<const Command*>& cmds) const {
    std::vector<std::string> lines;
    for (const auto* c : cmds) {
        std::string desc = c->description.empty() ? c->name : c->description;
        std::ostringstream line;
        line << "  " << c->name;
        // Pad to align descriptions (col 20).
        int pad = 20 - static_cast<int>(c->name.size());
        if (pad < 2) pad = 2;
        for (int i = 0; i < pad; ++i) line << ' ';
        line << "-  " << desc;
        lines.push_back(line.str());
    }
    return lines;
}

} // namespace completion

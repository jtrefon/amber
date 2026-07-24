// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef COMPLETION_COMMAND_H
#define COMPLETION_COMMAND_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace completion {

struct FlagSpec {
    std::string short_name;
    std::string long_name;
    std::string description;
    bool takes_value = false;
    std::string value_name;
};

struct ArgSpec {
    std::string name;
    std::string description;
    bool required = true;
    std::function<std::vector<std::string>(const std::string& partial)> complete;
};

struct Command {
    std::string name;
    std::vector<std::string> aliases;
    std::string args_usage;
    std::string description;
    std::vector<ArgSpec> args;
    std::vector<FlagSpec> flags;
    std::vector<std::unique_ptr<Command>> subcommands;

    std::function<void(const std::string& arg)> run;
    std::function<std::string()> current_value;

    // Non-owning pointer in case of back-links (vs unique_ptr for children).
    Command* parent = nullptr;

    void add_subcommand(std::unique_ptr<Command> child) {
        child->parent = this;
        subcommands.push_back(std::move(child));
    }

    const Command* find_subcommand(const std::string& name) const {
        for (const auto& c : subcommands) {
            if (c->name == name) return c.get();
            for (const auto& a : c->aliases)
                if (a == name) return c.get();
        }
        return nullptr;
    }

    const Command* find_subcommand_r(const std::string& name) const {
        for (const auto& c : subcommands) {
            if (c->name == name) return c.get();
            for (const auto& a : c->aliases)
                if (a == name) return c.get();
            const Command* deeper = c->find_subcommand_r(name);
            if (deeper) return deeper;
        }
        return nullptr;
    }

    // Collect this command and all descendants into a flat list.
    std::vector<const Command*> flatten() const {
        std::vector<const Command*> out;
        flatten_into(out);
        return out;
    }

    std::string usage() const {
        std::string u = "/" + name;
        if (!args_usage.empty()) u += " " + args_usage;
        return u;
    }

private:
    void flatten_into(std::vector<const Command*>& out) const {
        out.push_back(this);
        for (const auto& c : subcommands)
            c->flatten_into(out);
    }
};

} // namespace completion

#endif

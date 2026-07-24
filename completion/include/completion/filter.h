// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef COMPLETION_FILTER_H
#define COMPLETION_FILTER_H

#include "completion/command.h"

#include <string>
#include <vector>

namespace completion {

// Text before the first space (command name without leading '/').
std::string token(const std::string& input);

// Whether input has a space (argument mode).
bool has_arg(const std::string& input);

// Whether the line starts with '/'.
bool wants_open(const std::string& input);

// Commands whose name or alias starts with `tok`. Primary-name matches
// are listed before alias matches.
std::vector<const Command*> filter_top(const std::vector<std::unique_ptr<Command>>& commands,
                                        const std::string& tok);

// Longest common prefix of a set of strings.
std::string common_prefix(const std::vector<std::string>& names);

// Split input into the command path segments and the trailing partial arg.
// e.g. "/set detection l" → tokens=["set", "detection"], partial="l"
struct ParsedInput {
    std::vector<std::string> tokens;  // space-separated after leading /
    std::string partial;              // trailing partial (may be empty)
    bool ends_with_space = false;     // user typed a trailing space
};
ParsedInput parse_input(const std::string& input);

// Build a usage string "/name <args>" for display.
std::string usage_line(const Command& cmd);

} // namespace completion

#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/experience.h"

#include <sstream>

namespace agent {

MemoryRetriever::MemoryRetriever(const MemoryStore& store)
    : store_(store) {}

std::string MemoryRetriever::build_system_prompt_suffix(
    const std::string& user_message,
    size_t max_tokens) const {
    auto memories = store_.top_memories(20, user_message);
    auto skills = store_.top_skills(10, user_message);

    if (memories.empty() && skills.empty())
        return {};

    std::ostringstream out;
    out << "\n\n=== Learned Knowledge ===\n\n"
        << "Keep entries compact (under 200 tokens each) and descriptive.\n"
        << "Reference memories/skills by name — names are stable identifiers.\n";

    if (!memories.empty()) {
        out << "\nMemories:\n";
        for (const auto& m : memories) {
            std::string label = m.name.empty() ? m.content.substr(0, 40) : m.name;
            out << "  \"" << label << "\"\n"
                << "    " << m.content << "\n";
        }
    }

    if (!skills.empty()) {
        out << "\nSkills:\n";
        for (const auto& s : skills) {
            std::string label = s.name.empty() ? s.content.substr(0, 40) : s.name;
            out << "  \"" << label << "\"  [" << s.trigger_phrase << "]\n"
                << "    " << s.content << "\n";
        }
    }

    out << "\n=== End Learned Knowledge ===\n";
    std::string result = out.str();

    // Respect token budget: ~4 chars per token.
    if (max_tokens > 0 && result.size() > max_tokens * 4) {
        result.resize(max_tokens * 4);
        auto close = result.rfind("=== End Learned Knowledge ===");
        if (close == std::string::npos)
            result += "\n=== End Learned Knowledge ===\n";
    }
    return result;
}

} // namespace agent

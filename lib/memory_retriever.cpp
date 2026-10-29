#include "agent/experience.h"

#include <cstddef>
#include <string>

namespace agent {

namespace {

constexpr std::size_t kCharsPerToken = 4;

std::string memory_entry(const Memory& mem) {
    std::string label = mem.name.empty() ? mem.content.substr(0, 40) : mem.name;
    return "  \"" + label + "\"\n    " + mem.content + "\n";
}

} // namespace

MemoryRetriever::MemoryRetriever(const MemoryStore& store, ExperienceConfig cfg)
    : store_(store), cfg_(std::move(cfg)) {}

std::string MemoryRetriever::build_block() const {
    const std::uint64_t version = store_.version();
    if (has_cache_ && cached_version_ == version)
        return cached_;
    cached_ = render_block();
    cached_version_ = version;
    has_cache_ = true;
    return cached_;
}

std::string MemoryRetriever::render_block() const {
    auto memories = store_.top_memories(cfg_.max_memories, "");
    if (memories.empty())
        return {};

    std::string out = "\n\n=== Learned memories ===\n\n"
                      "Reference memories by name — names are stable identifiers.\n\nMemories:\n";
    // ~4 chars per token. The first entry is admitted even when it alone
    // exceeds the budget: a block with no memories is worse than a long one.
    const std::size_t budget =
        cfg_.max_prompt_tokens == 0 ? 0 : cfg_.max_prompt_tokens * kCharsPerToken;
    bool any = false;
    for (const auto& mem : memories) {
        std::string entry = memory_entry(mem);
        if (budget > 0 && any && out.size() + entry.size() + 40 > budget)
            break;
        out += entry;
        any = true;
    }
    out += "\n=== End learned memories ===\n";
    return out;
}

} // namespace agent

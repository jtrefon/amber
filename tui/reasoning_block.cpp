#include "reasoning_block.h"

namespace tui {

void ReasoningBlock::append(const std::string& delta) {
    // A delta arriving after a fold opens the next episode: a tool-calling turn
    // reasons once per LLM round-trip, and every episode must be visible again.
    if (folded) begin();
    buffer += delta;
}

std::string ReasoningBlock::fold() {
    if (folded) return {};
    folded = true;
    if (buffer.empty()) return {};
    std::size_t words = 1;
    for (char ch : buffer)
        if (ch == ' ') ++words;
    buffer.clear();
    return "[thought for " + std::to_string(words) + " words]";
}

} // namespace tui

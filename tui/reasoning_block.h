#pragma once

#include <string>

namespace tui {

// The live reasoning ("thinking") block of one chat window. Deltas stream in as
// dim text; when the answer (or the next tool call) starts, the block folds to
// a one-line summary so the scrollback stays readable while the trace stays
// visible.
struct ReasoningBlock {
    std::string buffer;
    bool folded = false;

    bool active() const noexcept { return !folded && !buffer.empty(); }
    void begin() noexcept { buffer.clear(); folded = false; }
    void append(const std::string& delta);
    std::string fold();
};

} // namespace tui

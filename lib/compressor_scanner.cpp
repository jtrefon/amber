
#include "agent/compressor.h"
#include "agent/agent_helpers.h"

#include <algorithm>
#include <string>

namespace agent {

namespace {

bool is_tool_call_message(const Message& msg) {
    return msg.role == "assistant" && !msg.tool_calls.is_null() && !msg.tool_calls.empty();
}

// A trailing result that belongs to a collapsed loop: the tool response, or a
// plain assistant message with no further calls.
bool is_loop_tail(const Message& msg) {
    return msg.role == "tool" ||
           (msg.role == "assistant" && (msg.tool_calls.is_null() || msg.tool_calls.empty()));
}

// Everything a loop running first_idx..i should drop, plus the trailing
// tool/assistant results it produced.
void collect_loop_range(const std::vector<Message>& history, size_t first_idx, size_t i,
                        std::vector<size_t>& to_remove) {
    for (size_t j = first_idx; j <= i; ++j)
        to_remove.push_back(j);
    for (size_t j = i + 1; j < history.size(); ++j) {
        if (!is_loop_tail(history[j]))
            break;
        to_remove.push_back(j);
    }
}

// Drop the collected indices (highest first, so the earlier ones stay valid)
// and leave one note at the first removed position.
void apply_collapse(std::vector<Message>& history, std::vector<size_t>& to_remove,
                    const std::vector<std::string>& collapse_notes) {
    if (to_remove.empty())
        return;
    std::sort(to_remove.begin(), to_remove.end());
    to_remove.erase(std::unique(to_remove.begin(), to_remove.end()), to_remove.end());
    const size_t first = to_remove.front();
    for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it)
        history.erase(history.begin() + static_cast<ptrdiff_t>(*it));
    for (const auto& note : collapse_notes) {
        Message note_msg;
        note_msg.role = "assistant";
        note_msg.content = "[loop collapsed] " + note;
        history.insert(history.begin() + static_cast<ptrdiff_t>(first), note_msg);
    }
}

} // namespace

void collapse_loops(std::vector<Message>& history) {
    if (history.size() < 4)
        return;

    std::string last_key;
    int count = 0;
    size_t first_idx = 0;
    std::vector<size_t> to_remove;
    std::vector<std::string> collapse_notes;

    for (size_t i = 0; i < history.size(); ++i) {
        const auto& msg = history[i];
        if (!is_tool_call_message(msg))
            continue;

        const std::string key = fingerprint_tool_calls(msg.tool_calls);
        if (key != last_key || key.empty()) {
            last_key = key;
            count = 1;
            first_idx = i;
            continue;
        }

        ++count;
        if (count < 3)
            continue;
        // A loop running from first_idx through i: drop it and the results it
        // produced, and record what was collapsed.
        collect_loop_range(history, first_idx, i, to_remove);
        collapse_notes.push_back("turns " + std::to_string(first_idx) + "-" + std::to_string(i) +
                                 ": tool loop detected, " + std::to_string(count) +
                                 " identical calls collapsed");
        last_key.clear();
        count = 0;
    }

    apply_collapse(history, to_remove, collapse_notes);
}

} // namespace agent

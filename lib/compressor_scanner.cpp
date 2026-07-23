// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/compressor.h"

#include <algorithm>
#include <string>

namespace agent {

void collapse_loops(std::vector<Message>& history) {
    if (history.size() < 4) return;

    std::string last_key;
    int count = 0;
    size_t first_idx = 0;

    std::vector<size_t> to_remove;
    std::vector<std::string> collapse_notes;

    // Only examine assistant messages with tool_calls.
    // Non-tool-call messages are skipped so they don't break sequence tracking.
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& msg = history[i];

        bool is_tool_call = (msg.role == "assistant" &&
                             !msg.tool_calls.is_null() &&
                             !msg.tool_calls.empty());
        if (!is_tool_call) continue;

        std::string key;
        for (const auto& tc : msg.tool_calls) {
            auto fn = tc.value("function", json::object());
            key += fn.value("name", "") + ":";
            key += fn.value("arguments", "") + "|";
        }

        if (key == last_key && !key.empty()) {
            ++count;
            if (count >= 3) {
                // Found a loop from first_idx through i.
                // Remove all messages in that range.
                for (size_t j = first_idx; j <= i; ++j)
                    to_remove.push_back(j);
                // Also remove trailing tool/assistant result messages.
                for (size_t j = i + 1; j < history.size(); ++j) {
                    if (history[j].role == "tool" ||
                        (history[j].role == "assistant" &&
                         (history[j].tool_calls.is_null() ||
                          history[j].tool_calls.empty()))) {
                        to_remove.push_back(j);
                    } else break;
                }
                collapse_notes.push_back(
                    "turns " + std::to_string(first_idx) + "-" +
                    std::to_string(i) +
                    ": tool loop detected, " +
                    std::to_string(count) +
                    " identical calls collapsed");
                last_key.clear();
                count = 0;
            }
        } else {
            last_key = key;
            count = 1;
            first_idx = i;
        }
    }

    if (to_remove.empty()) return;

    std::sort(to_remove.begin(), to_remove.end());
    to_remove.erase(std::unique(to_remove.begin(), to_remove.end()),
                    to_remove.end());

    size_t first = to_remove.front();
    for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it)
        history.erase(history.begin() + static_cast<ptrdiff_t>(*it));

    for (const auto& note : collapse_notes) {
        Message note_msg;
        note_msg.role = "assistant";
        note_msg.content = "[loop collapsed] " + note;
        history.insert(history.begin() + static_cast<ptrdiff_t>(first),
                       note_msg);
    }
}

} // namespace agent

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_CONTEXT_H
#define AGENT_CONTEXT_H

#include <cstddef>
#include <deque>
#include <vector>

#include "agent/llm.h"
#include "agent/tool.h"

namespace agent {

// Immutable, stack-based conversation context.
//
// Messages are sealed on push — once on the stack they can never be
// modified. The only operations are push (append to top), pop (remove
// from bottom, for compression), and get_all (read-only). Token count
// is maintained as a cached invariant, updated incrementally on push
// and recomputed on pop/clear.
//
// This replaces the old mutable std::vector<Message> history_ that
// allowed in-place mutation via .back() references and front-insertion.
class Context {
public:
    // Push a sealed message onto the top of the stack.
    // The message is moved in — caller loses ownership.
    void push(Message msg) noexcept {
        token_count_ += message_tokens(msg);
        stack_.push_back(std::move(msg));
    }

    // Pop `n` messages from the bottom of the stack and return them.
    // Used by compression to remove old turns. If `n` exceeds the
    // stack size, all messages are returned and the stack is cleared.
    // Token count is recomputed after removal.
    std::vector<Message> pop(size_t n) noexcept {
        if (n > stack_.size()) n = stack_.size();
        std::vector<Message> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(std::move(stack_.front()));
            stack_.pop_front();
        }
        if (!stack_.empty())
            recompute_token_count();
        else
            token_count_ = 0;
        return out;
    }

    // Read-only view of the entire stack. No caller can modify
    // messages through this reference.
    const std::deque<Message>& get_all() const noexcept { return stack_; }

    // Cached total token count of all messages in the stack.
    // Accounts for content, reasoning, tool_calls JSON, and
    // per-message formatting overhead.
    size_t token_count() const noexcept { return token_count_; }

    size_t size() const noexcept { return stack_.size(); }
    bool empty() const noexcept { return stack_.empty(); }

    void clear() noexcept {
        stack_.clear();
        token_count_ = 0;
    }

private:
    std::deque<Message> stack_;
    size_t token_count_ = 0;

    static constexpr int kOverhead = 4; // per-message chat template overhead

    static size_t message_tokens(const Message& msg) noexcept {
        size_t n = (msg.content.size() + msg.reasoning.size()) / 4;
        if (!msg.tool_calls.is_null())
            n += msg.tool_calls.dump().size() / 4;
        return n + kOverhead;
    }

    void recompute_token_count() noexcept {
        token_count_ = 0;
        for (const auto& msg : stack_)
            token_count_ += message_tokens(msg);
    }
};

} // namespace agent

#endif // AGENT_CONTEXT_H

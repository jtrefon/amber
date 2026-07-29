// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_CONTEXT_H
#define AGENT_CONTEXT_H

#include <cstddef>
#include <deque>
#include <functional>
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

    // Atomically replace the entire stack with a compressed history.
    // Used by compression to swap the full context in one operation.
    // The old stack is destroyed after the replacement — no intermediate
    // window where the context is empty. Token count is recomputed.
    void replace(std::vector<Message> new_msgs) noexcept {
        // Build a new deque in-place, then swap.
        std::deque<Message> replacement;
        for (auto& m : new_msgs)
            replacement.push_back(std::move(m));
        stack_.swap(replacement);
        recompute_token_count();
    }

    // Pop the most recently pushed message from the top of the stack.
    // Used by compression to remove a request message after the LLM call,
    // keeping the KV cache valid for subsequent turns.
    void pop_back() noexcept {
        if (stack_.empty()) return;
        token_count_ -= message_tokens(stack_.back());
        stack_.pop_back();
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

// Lightweight one-to-many event source for context-change notifications.
// Multiple subscribers receive (token_count, message_count) on every
// push/pop/clear. Subscriber order is not guaranteed.
class ContextEventSource {
public:
    using Callback = std::function<void(size_t tokens, size_t msgs)>;

    void subscribe(Callback cb) {
        subs_.push_back(std::move(cb));
    }

    void publish(size_t tokens, size_t msgs) const {
        for (const auto& cb : subs_)
            if (cb) cb(tokens, msgs);
    }

private:
    std::vector<Callback> subs_;
};

} // namespace agent

#endif // AGENT_CONTEXT_H

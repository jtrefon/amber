
#ifndef AGENT_CONTEXT_H
#define AGENT_CONTEXT_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "agent/llm.h"
#include "agent/tool.h"

namespace agent {

// Token estimation for a single message (chars/4 plus per-message overhead).
// Single source of truth: Context's cached counter and the compression
// budget enforcement both use this.
inline constexpr int kPerMessageTokenOverhead = 4;

inline size_t message_tokens(const Message& msg) noexcept {
    size_t n = (msg.content.size() + msg.reasoning.size()) / 4;
    if (!msg.tool_calls.is_null())
        n += msg.tool_calls.dump().size() / 4;
    return n + kPerMessageTokenOverhead;
}

inline size_t estimate_tokens(const std::vector<Message>& msgs) noexcept {
    size_t n = 0;
    for (const auto& msg : msgs) n += message_tokens(msg);
    return n;
}

// Pure-stack conversation context with hash-chain integrity.
//
// Messages are sealed on push — once on the stack they can never be
// modified. The only operations are push (append to top), pop (remove
// from top — LIFO), clear (remove all), and get_all (read-only).
// Token count is maintained as a cached invariant, updated
// incrementally on push/pop and recomputed on clear.
//
// A FNV-1a hash chain links every message to its predecessor:
//
//   h_0 = FNV(0              || msg_0)
//   h_1 = FNV(h_0            || msg_1)
//   h_2 = FNV(h_1            || msg_2)
//   ...
//   chain_hash_ = h_{n-1}
//
// get_all() recomputes the chain from the stored messages and asserts
// the final hash matches chain_hash_.  Any in-place modification of a
// sealed message (const_cast, rogue replace, etc.) breaks every
// subsequent link and triggers an assertion.  Each message's hash is
// stored in a parallel deque so that pop() restores chain_hash_ in O(1)
// without a full recompute.
class Context {
public:
    // Push a sealed message onto the top of the stack.
    void push(Message msg) noexcept {
        uint64_t h = chain_hash(chain_hash_, msg);
        hashes_.push_back(h);
        chain_hash_ = h;
        token_count_ += ::agent::message_tokens(msg);
        stack_.push_back(std::move(msg));
    }

    // Pop the top (most recently pushed) message off the stack.
    // This is the LIFO counterpart to push().  Returns the removed
    // message so the caller can inspect or discard it.
    // Precondition: stack is not empty.
    Message pop() noexcept {
        assert(!stack_.empty());
        hashes_.pop_back();
        chain_hash_ = hashes_.empty() ? 0 : hashes_.back();
        token_count_ -= ::agent::message_tokens(stack_.back());
        Message out = std::move(stack_.back());
        stack_.pop_back();
        return out;
    }

    // Read-only view of the entire stack.  Verifies the hash chain
    // before returning so any in-place mutation is caught immediately.
    const std::deque<Message>& get_all() const noexcept {
        assert(verify_chain());
        return stack_;
    }

    size_t token_count() const noexcept { return token_count_; }
    size_t size() const noexcept { return stack_.size(); }
    bool empty() const noexcept { return stack_.empty(); }

    void clear() noexcept {
        stack_.clear();
        hashes_.clear();
        token_count_ = 0;
        chain_hash_ = 0;
    }

private:
    std::deque<Message> stack_;
    std::deque<uint64_t> hashes_;    // one chained hash per message
    size_t token_count_ = 0;
    uint64_t chain_hash_ = 0;        // hash of the last message (== hashes_.back())

    // ------------------------------------------------------------------
    // Hash-chain
    // ------------------------------------------------------------------

    static uint64_t fnv1a(const std::string& s) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : s) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }

    // input = prev_hash (8 bytes LE) || role || \0 || content || \0
    //         || reasoning || \0 || tool_call_id || \0 || name
    //         [|| \0 || tool_calls.dump()]
    static uint64_t chain_hash(uint64_t prev, const Message& msg) noexcept {
        auto cap = 8 + msg.role.size() + 1 + msg.content.size() + 1
                     + msg.reasoning.size() + 1
                     + msg.tool_call_id.size() + 1 + msg.name.size() + 1;
        if (!msg.tool_calls.is_null())
            cap += msg.tool_calls.dump().size() + 1;
        std::string buf;
        buf.reserve(cap);
        for (size_t i = 0; i < 8; ++i)
            buf.push_back(static_cast<char>((prev >> (i * 8)) & 0xFF));
        auto append = [&](const std::string& s) { buf += s; buf += '\0'; };
        append(msg.role);
        append(msg.content);
        append(msg.reasoning);
        append(msg.tool_call_id);
        append(msg.name);
        if (!msg.tool_calls.is_null()) append(msg.tool_calls.dump());
        return fnv1a(buf);
    }

    bool verify_chain() const noexcept {
        uint64_t expected = 0;
        for (const auto& msg : stack_)
            expected = chain_hash(expected, msg);
        return expected == chain_hash_;
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

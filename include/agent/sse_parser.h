
#ifndef AGENT_SSE_PARSER_H
#define AGENT_SSE_PARSER_H

#include "agent/llm.h"
#include <functional>
#include <string>
#include <utility>

namespace agent {

// A single response never carries more tool calls than this; a sparse index
// delta (malicious or buggy server) must not allocate unbounded slots.
inline constexpr int kMaxToolCallsPerMessage = 64;

// raw_body_ is diagnostics-only; cap it so a long stream cannot grow memory
// without bound.
inline constexpr std::size_t kMaxRawBodyBytes = std::size_t(64) * 1024;

// Mutable state threaded through the streaming write callback so SSE events are
// parsed and dispatched incrementally, as bytes arrive from the network.
struct SseState {
    std::string buffer;        // partial SSE line carried across writes
    bool in_think = false;     // inside an inline <think> ... </think> span
    std::string pending;       // tail kept back for tag-boundary lookahead
    bool finished = false;     // guards against double finalize
    long prompt_tokens = -1;   // from the final usage chunk
    long completion_tokens = -1;
};

// Incremental Server-Sent-Events parser for streamed chat completions. It owns
// the mutable parse state (partial lines, <think> segmentation, usage stats) and
// assembles the final Message, invoking on_chunk for every parsed delta so UIs
// can render live.
class StreamParser {
public:
    using ChunkSink = std::function<void(const StreamChunk&)>;

    StreamParser(Message& out, const ChunkSink& on_chunk,
                 std::string  debug_path)
        : out_(out), on_chunk_(on_chunk), debug_path_(std::move(debug_path)) {}

    // libcurl write callback entry point: feed raw SSE bytes, parse whole lines.
    size_t on_write(const char* data, size_t size, size_t nmemb);

    // Drain any trailing partial line and emit the terminal chunk. Idempotent;
    // safe to call on [DONE] and again after transfer completes.
    void finalize();

    long prompt_tokens() const { return st_.prompt_tokens; }
    long completion_tokens() const { return st_.completion_tokens; }

    // The bounded raw byte accumulation (error diagnostics).
    const std::string& raw_body() const { return raw_body_; }

    void dispatch_event(const std::string& data);
    void segment_think(const std::string& text, StreamChunk& chunk);

    Message& out_;
    const ChunkSink& on_chunk_;
    std::string debug_path_;
    SseState st_;
    std::string raw_body_;  // all bytes received; used for error diagnostics
};

} // namespace agent

#endif // AGENT_SSE_PARSER_H

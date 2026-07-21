// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_SSE_PARSER_H
#define AGENT_SSE_PARSER_H

#include "agent/llm.h"
#include <functional>
#include <string>
#include <utility>

namespace agent {

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

    void dispatch_event(const std::string& data);
    void segment_think(const std::string& text, StreamChunk& chunk);

    Message& out_;
    const ChunkSink& on_chunk_;
    std::string debug_path_;
    SseState st_;
};

} // namespace agent

#endif // AGENT_SSE_PARSER_H

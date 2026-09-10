
#ifndef AGENT_STREAM_DECODER_H
#define AGENT_STREAM_DECODER_H

#include "agent/llm.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace agent {

// A single response never carries more tool calls than this; a sparse index
// delta (malicious or buggy server) must not allocate unbounded slots.
inline constexpr int kMaxToolCallsPerMessage = 64;

// raw_body_ is diagnostics-only; cap it so a long stream cannot grow memory
// without bound.
inline constexpr std::size_t kMaxRawBodyBytes = std::size_t(64) * 1024;

// Port: incremental decoder for a streamed chat response. Subclasses own the
// wire-specific event state machine (decode_payload/decode_end); the base owns
// the shared framing: line splitting, raw-byte capture, debug logging, the
// terminal done chunk, and the usage counters.
class StreamDecoder {
public:
    using ChunkSink = std::function<void(const StreamChunk&)>;

    StreamDecoder(Message& out, ChunkSink on_chunk, std::string debug_path);
    virtual ~StreamDecoder() = default;

    // libcurl write callback entry point: feed raw bytes, parse whole lines.
    std::size_t on_write(const char* data, std::size_t size, std::size_t nmemb);

    // Drain any trailing partial line and emit the terminal chunk. Idempotent;
    // safe to call on [DONE] and again after transfer completes.
    void finalize();

    long prompt_tokens() const noexcept { return prompt_tokens_; }
    long completion_tokens() const noexcept { return completion_tokens_; }

    // The bounded raw byte accumulation (error diagnostics).
    const std::string& raw_body() const noexcept { return raw_body_; }

protected:
    // Decode one `data:` payload (the text after "data:", leading blanks
    // trimmed). The terminal "[DONE]" marker is handled by the base.
    virtual void decode_payload(const std::string& payload) = 0;

    // Flush wire-specific pending state and compact the assembled message.
    virtual void decode_end() = 0;

    // Invoke the sink when the chunk carries anything.
    void emit(const StreamChunk& chunk);

    Message& out_;
    ChunkSink on_chunk_;   // stored by value: an `auto` lambda binds through a
                           // temporary std::function, and a reference member
                           // would dangle on the first content delta.
    std::string debug_path_;
    long prompt_tokens_ = -1;      // from the final usage event
    long completion_tokens_ = -1;

private:
    std::string buffer_;    // partial SSE line carried across writes
    std::string raw_body_;  // all bytes received; used for error diagnostics
    bool finished_ = false; // guards against double finalize
};

} // namespace agent

#endif // AGENT_STREAM_DECODER_H


#include "agent/stream_decoder.h"
#include "agent/debug_log.h"

namespace agent {

StreamDecoder::StreamDecoder(Message& out, ChunkSink on_chunk,
                             std::string debug_path)
    : out_(out), on_chunk_(std::move(on_chunk)),
      debug_path_(std::move(debug_path)) {}

std::size_t StreamDecoder::on_write(const char* data, std::size_t size,
                                    std::size_t nmemb) {
    const std::string raw(data, size * nmemb);
    if (raw_body_.size() < kMaxRawBodyBytes)
        raw_body_.append(raw.substr(0, kMaxRawBodyBytes - raw_body_.size()));
    if (!debug_path_.empty())
        debug_log(debug_path_, "sse-raw", raw);
    buffer_.append(raw);

    std::size_t nl = 0;
    while ((nl = buffer_.find('\n')) != std::string::npos) {
        std::string line = buffer_.substr(0, nl);
        buffer_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data:", 0) != 0) continue;
        std::string payload = line.substr(5);
        const std::size_t p = payload.find_first_not_of(" \t");
        if (p == std::string::npos) continue;  // blank payload: nothing to do
        payload = payload.substr(p);
        if (payload == "[DONE]") {
            finalize();
            continue;
        }
        decode_payload(payload);
    }
    return size * nmemb;
}

void StreamDecoder::finalize() {
    if (finished_) return;
    finished_ = true;
    decode_end();
    if (on_chunk_) {
        StreamChunk end;
        end.done = true;
        on_chunk_(end);
    }
}

void StreamDecoder::emit(const StreamChunk& chunk) {
    if (!on_chunk_) return;
    if (chunk.delta.empty() && chunk.reasoning.empty() &&
        chunk.tool_calls.is_null())
        return;
    on_chunk_(chunk);
}

} // namespace agent

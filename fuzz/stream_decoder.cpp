// Fuzz the SSE framing in StreamDecoder.
//
// This is the libcurl write callback: raw bytes off a socket, split into lines across
// arbitrary write boundaries, accumulated into a bounded raw body. The line splitting,
// the kMaxRawBodyBytes cap and the trailing-partial-line drain are all arithmetic on
// attacker-influenced sizes, which is exactly the shape that hides an overflow.
//
// A real provider decoder parses each payload as JSON, so decode_payload does too --
// otherwise the fuzzer would only ever reach the framing and never the parse behind it.

#include <cstddef>
#include <cstdint>
#include <string>

#include "agent/stream_decoder.h"

namespace {

class FuzzDecoder : public agent::StreamDecoder {
public:
    explicit FuzzDecoder(agent::Message& message)
        : agent::StreamDecoder(message, [](const agent::StreamChunk&) {}, "") {}

protected:
    void decode_payload(const std::string& payload) override {
        try {
            const agent::json parsed = agent::json::parse(payload);
            (void)parsed;
        } catch (...) {
            // A malformed payload is an expected outcome, not a finding: the provider
            // decoders treat it as recoverable so the model can be told.
        }
    }

    void decode_end() override {}
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    agent::Message message;
    FuzzDecoder decoder(message);

    // Deliberately not the real libcurl call shape (which passes nmemb=1 for a whole
    // buffer): one call with an arbitrary size is the harsher case, and it also covers
    // the zero-length write that terminates some transfers.
    decoder.on_write(reinterpret_cast<const char*>(data), size, 1);
    decoder.finalize();
    (void)decoder.prompt_tokens();
    (void)decoder.completion_tokens();
    (void)decoder.raw_body().size();
    return 0;
}

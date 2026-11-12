// Fuzz the external plugin protocol: newline framing and the initialize/tool
// reply decoders. The bytes arrive from a plugin subprocess, which the security
// model treats as untrusted; the unit suite pins the shapes and libFuzzer
// explores the rest.
//
// The 64 KiB cap is exercised from a synthesized buffer rather than a seed: the
// fuzz job runs with FUZZ_MAX_LEN=4096, so a 64 KiB corpus input would never be
// a useful mutation source, and a huge single-line seed is exactly what the
// obfuscation guard is built to notice.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "agent/plugin_protocol.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);

    const agent::PluginInitReply init = agent::plugin_decode_initialize(text);
    (void)init.ok;
    (void)init.error.size();

    const agent::PluginCallReply call = agent::plugin_decode_call(text);
    (void)call.decoded;
    (void)call.ok;
    (void)call.output.size();
    (void)call.error.size();
    if (!call.meta.is_null())
        (void)call.meta.dump().size();

    // Drain at most a few frames: a buffer of many lines must not turn one
    // fuzz input into unbounded work through repeated substring erases.
    std::string buffer = text;
    for (int i = 0; i < 8; ++i) {
        std::string line;
        if (agent::plugin_take_line(buffer, line) != agent::PluginLineState::Ready)
            break;
        (void)line.size();
    }

    static std::string oversized(agent::kPluginMaxLineBytes + 1, 'x');
    std::string ignored;
    if (agent::plugin_take_line(oversized, ignored) != agent::PluginLineState::TooLarge)
        std::abort();
    return 0;
}

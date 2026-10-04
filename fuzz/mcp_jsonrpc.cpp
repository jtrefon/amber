// Fuzz the MCP JSON-RPC line decoder.
//
// Every byte here arrives from an MCP server over a socket or a subprocess pipe, and
// the security model treats it as untrusted. The unit suite exercises a handful of
// hand-written shapes; libFuzzer explores the rest.
//
// Only decode is fuzzed: the encode functions take already-structured input, so there
// is nothing arbitrary to hand them.

#include <cstddef>
#include <cstdint>
#include <string>

#include "agent/mcp_transport.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string line(reinterpret_cast<const char*>(data), size);
    std::optional<agent::McpMessage> message = agent::mcp_decode_line(line);
    if (!message)
        return 0;

    // Touch what a caller reads next, so the fuzzer reaches the conversions and the
    // accessors rather than stopping at "the line parsed". to_text() formats an
    // attacker-controlled message and code.
    (void)message->is_notification();
    (void)message->is_response();
    (void)message->method.size();
    (void)message->params.empty();
    (void)message->result.has_value();
    if (message->error)
        (void)message->error->to_text().size();
    return 0;
}

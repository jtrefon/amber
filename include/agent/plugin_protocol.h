#ifndef AGENT_PLUGIN_PROTOCOL_H
#define AGENT_PLUGIN_PROTOCOL_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

namespace agent {

// The external plugin protocol is one JSON object per line over stdio. Those
// bytes come from a child process the harness does not control, so framing and
// decoding are pure functions here: the manager moves bytes and polls the fd,
// and the fuzz target drives this unit without a subprocess.
inline constexpr std::size_t kPluginMaxLineBytes = std::size_t{64} * 1024;

enum class PluginLineState { Ready, Incomplete, TooLarge };

// Take one newline-terminated line out of `buffer`. `line` is set only on
// Ready. An unterminated buffer past the cap reports TooLarge, so the caller
// fails the exchange instead of letting a broken plugin grow it without bound.
PluginLineState plugin_take_line(std::string& buffer, std::string& line);

// Decoded replies. Both fail closed: discarded JSON, a non-object document or a
// missing/ill-typed result becomes an error string, never an exception.
struct PluginInitReply {
    bool ok = false;
    std::string error;
};

PluginInitReply plugin_decode_initialize(const std::string& line);

struct PluginCallReply {
    // The line was a JSON object with a `result`. A decoded reply can still be
    // an unsuccessful call; only `decoded == false` means the protocol broke.
    bool decoded = false;
    bool ok = false;
    std::string output;
    // The decode failure when `decoded` is false, empty otherwise. A plugin's
    // own error text lands in `output`, matching ToolResult.
    std::string error;
    nlohmann::json meta;
};

PluginCallReply plugin_decode_call(const std::string& line);

} // namespace agent

#endif // AGENT_PLUGIN_PROTOCOL_H

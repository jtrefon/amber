#include "agent/plugin_protocol.h"

#include "agent/json_read.h"

namespace agent {

PluginLineState plugin_take_line(std::string& buffer, std::string& line) {
    const std::size_t nl = buffer.find('\n');
    if (nl == std::string::npos)
        return buffer.size() > kPluginMaxLineBytes ? PluginLineState::TooLarge
                                                   : PluginLineState::Incomplete;
    if (nl > kPluginMaxLineBytes)
        return PluginLineState::TooLarge;
    line = buffer.substr(0, nl);
    buffer.erase(0, nl + 1);
    return PluginLineState::Ready;
}

PluginInitReply plugin_decode_initialize(const std::string& line) {
    const nlohmann::json resp = nlohmann::json::parse(line, nullptr, false);
    if (resp.is_discarded() || !resp.is_object() || !resp.contains("result") ||
        !read_bool(resp["result"], "ok", false))
        return {false, "initialize rejected"};
    return {true, ""};
}

PluginCallReply plugin_decode_call(const std::string& line) {
    const nlohmann::json resp = nlohmann::json::parse(line, nullptr, false);
    if (resp.is_discarded() || !resp.is_object() || !resp.contains("result")) {
        PluginCallReply failed;
        failed.error = "malformed plugin response";
        return failed;
    }
    const nlohmann::json& result = resp["result"];
    PluginCallReply out;
    out.decoded = true;
    out.ok = read_bool(result, "ok", false);
    out.output = read_string(result, "output", "");
    if (!out.ok && out.output.empty())
        out.output = read_string(result, "error", "");
    if (result.is_object() && result.contains("meta"))
        out.meta = result["meta"];
    return out;
}

} // namespace agent

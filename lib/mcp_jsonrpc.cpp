
#include "agent/mcp_transport.h"

namespace agent {

std::string McpError::to_text() const {
    std::string out = "mcp error " + std::to_string(code) + ": " + message;
    if (!data.is_null() && !data.empty())
        out += " (" + data.dump() + ")";
    return out;
}

namespace {

json rpc_object(const char* method, const json& params, const json* id, const json* result,
                const McpError* error) {
    json obj = {{"jsonrpc", "2.0"}};
    if (id)
        obj["id"] = *id;
    if (method)
        obj["method"] = method;
    if (!params.is_null())
        obj["params"] = params;
    if (result)
        obj["result"] = *result;
    if (error) {
        json e = {{"code", error->code}, {"message", error->message}};
        if (!error->data.is_null())
            e["data"] = error->data;
        obj["error"] = e;
    }
    return obj;
}

} // namespace

std::string mcp_encode_request(const McpRequest& req) {
    json id = req.id;
    return rpc_object(req.method.c_str(), req.params, &id, nullptr, nullptr).dump();
}

std::string mcp_encode_notification(const std::string& method, const json& params) {
    return rpc_object(method.c_str(), params, nullptr, nullptr, nullptr).dump();
}

std::string mcp_encode_response(int id, const json& result) {
    json jid = id;
    return rpc_object(nullptr, json::object(), &jid, &result, nullptr).dump();
}

std::string mcp_encode_error_response(int id, const McpError& error) {
    json jid = id;
    return rpc_object(nullptr, json::object(), &jid, nullptr, &error).dump();
}

// The JSON-RPC error object, when the message carries one.
//
// Every field is type-checked before it is read. json::value() THROWS on a
// type mismatch rather than falling back to its default, and this input is
// untrusted: the fuzzer took the process down with {"error":{"code":")-.4"}}.
// A mis-typed field is dropped, not fatal -- the error is still reported.
std::optional<McpError> decode_error(const json& obj) {
    if (!obj.contains("error") || !obj["error"].is_object())
        return std::nullopt;
    const json& e = obj["error"];
    McpError err;
    if (e.contains("code") && e["code"].is_number_integer())
        err.code = e["code"].get<int>();
    if (e.contains("message") && e["message"].is_string())
        err.message = e["message"].get<std::string>();
    if (e.contains("data"))
        err.data = e["data"];
    return err;
}

// Copy the fields this transport understands out of a decoded object.
McpMessage decode_fields(const json& obj) {
    McpMessage msg;
    if (obj.contains("id"))
        msg.id = obj["id"];
    if (obj.contains("method") && obj["method"].is_string())
        msg.method = obj["method"].get<std::string>();
    if (obj.contains("params") && obj["params"].is_object())
        msg.params = obj["params"];
    if (obj.contains("result"))
        msg.result = obj["result"];
    if (auto err = decode_error(obj))
        msg.error = *err;
    return msg;
}

std::optional<McpMessage> mcp_decode_line(const std::string& line) {
    json obj;
    try {
        obj = json::parse(line);
    } catch (...) {
        return std::nullopt;
    }
    if (!obj.is_object())
        return std::nullopt;
    if (obj.contains("jsonrpc") && obj["jsonrpc"] != "2.0")
        return std::nullopt;

    McpMessage msg = decode_fields(obj);
    if (msg.method.empty() && !msg.id.has_value())
        return std::nullopt;
    return msg;
}

} // namespace agent

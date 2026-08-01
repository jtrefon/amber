
#include "agent/mcp_transport.h"

namespace agent {

std::string McpError::to_text() const {
    std::string out = "mcp error " + std::to_string(code) + ": " + message;
    if (!data.is_null() && !data.empty()) out += " (" + data.dump() + ")";
    return out;
}

namespace {

json rpc_object(const char* method, const json& params, const json* id,
                const json* result, const McpError* error) {
    json obj = {{"jsonrpc", "2.0"}};
    if (id) obj["id"] = *id;
    if (method) obj["method"] = method;
    if (!params.is_null()) obj["params"] = params;
    if (result) obj["result"] = *result;
    if (error) {
        json e = {{"code", error->code}, {"message", error->message}};
        if (!error->data.is_null()) e["data"] = error->data;
        obj["error"] = e;
    }
    return obj;
}

} // namespace

std::string mcp_encode_request(const McpRequest& req) {
    json id = req.id;
    return rpc_object(req.method.c_str(), req.params, &id, nullptr,
                      nullptr)
        .dump();
}

std::string mcp_encode_notification(const std::string& method,
                                    const json& params) {
    return rpc_object(method.c_str(), params, nullptr, nullptr, nullptr)
        .dump();
}

std::string mcp_encode_response(int id, const json& result) {
    json jid = id;
    return rpc_object(nullptr, json::object(), &jid, &result, nullptr)
        .dump();
}

std::string mcp_encode_error_response(int id, const McpError& error) {
    json jid = id;
    return rpc_object(nullptr, json::object(), &jid, nullptr, &error)
        .dump();
}

std::optional<McpMessage> mcp_decode_line(const std::string& line) {
    json obj;
    try {
        obj = json::parse(line);
    } catch (...) {
        return std::nullopt;
    }
    if (!obj.is_object()) return std::nullopt;
    if (obj.contains("jsonrpc") && obj["jsonrpc"] != "2.0")
        return std::nullopt;

    McpMessage msg;
    if (obj.contains("id")) msg.id = obj["id"];
    if (obj.contains("method") && obj["method"].is_string())
        msg.method = obj["method"].get<std::string>();
    if (obj.contains("params") && obj["params"].is_object())
        msg.params = obj["params"];
    if (obj.contains("result")) msg.result = obj["result"];
    if (obj.contains("error") && obj["error"].is_object()) {
        McpError err;
        err.code = obj["error"].value("code", 0);
        err.message = obj["error"].value("message", "");
        if (obj["error"].contains("data")) err.data = obj["error"]["data"];
        msg.error = err;
    }
    if (msg.method.empty() && !msg.id.has_value()) return std::nullopt;
    return msg;
}

} // namespace agent

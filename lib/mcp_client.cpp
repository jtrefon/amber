
#include "agent/mcp_client.h"

#include <algorithm>

namespace agent {

namespace {

constexpr size_t kToolCap = static_cast<size_t>(64) * 1024;
constexpr size_t kResourceCap = static_cast<size_t>(256) * 1024;
constexpr size_t kPromptCap = static_cast<size_t>(32) * 1024;
constexpr int kMaxPages = 10;
constexpr const char* kProtocolVersion = "2025-06-18";

std::string append_capped(std::string& out, const std::string& text, size_t cap) {
    if (out.size() >= cap)
        return out;
    size_t room = cap - out.size();
    if (text.size() <= room) {
        out += text;
    } else {
        out += text.substr(0, room);
        out += "\n[truncated: " + std::to_string(text.size() - room) + " bytes]";
    }
    return out;
}

json init_params() {
    return {{"protocolVersion", kProtocolVersion},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "amber"}, {"version", "1.0.0"}}}};
}

} // namespace

MCPClient::MCPClient(std::string server_name, std::unique_ptr<McpTransport> transport,
                     std::string transport_error, const CancellationToken* cancel_token)
    : name_(std::move(server_name)), transport_(std::move(transport)),
      transport_error_(std::move(transport_error)), cancel_token_(cancel_token) {
    if (transport_) {
        transport_->set_on_server_message(
            [this](const McpMessage& m) { handle_server_message(m); });
    }
}

std::string MCPClient::connect() {
    error_.clear();
    connected_ = false;
    tools_.clear();
    resources_.clear();
    prompts_.clear();
    return do_connect();
}

namespace {

// Copy serverInfo and the capability flags out of the initialize result.
void apply_server_info(const json& result, McpServerInfo& info, McpCapabilities& caps) {
    const json si = result.value("serverInfo", json::object());
    info.name = si.value("name", "");
    info.title = si.value("title", "");
    info.version = si.value("version", "");

    const json c = result.value("capabilities", json::object());
    caps.has_tools = c.contains("tools");
    caps.has_resources = c.contains("resources");
    caps.has_prompts = c.contains("prompts");
    caps.has_logging = c.contains("logging");
    caps.tools_list_changed = c.value("tools", json::object()).value("listChanged", false);
    caps.resources_list_changed = c.value("resources", json::object()).value("listChanged", false);
    caps.prompts_list_changed = c.value("prompts", json::object()).value("listChanged", false);
}

} // namespace

std::string MCPClient::do_connect() {
    if (!transport_) {
        error_ = transport_error_.empty() ? "no transport" : transport_error_;
        return error_;
    }
    const auto init = transport_->request(next_id(), "initialize", init_params());
    if (init.status == McpTransportStatus::Timeout) {
        error_ = "initialize timed out";
        return error_;
    }
    if (!init.message) {
        error_ = transport_->failure_reason().empty() ? "initialize failed"
                                                      : transport_->failure_reason();
        return error_;
    }
    if (init.message->error) {
        error_ = init.message->error->to_text();
        return error_;
    }
    const json result = init.message->result.value_or(json::object());
    const std::string server_version = result.value("protocolVersion", "");
    if (server_version != kProtocolVersion) {
        error_ =
            "unsupported protocol version: " + (server_version.empty() ? "(none)" : server_version);
        return error_;
    }
    apply_server_info(result, server_info_, caps_);
    transport_->notify("notifications/initialized", json::object());
    connected_ = true;
    return refresh();
}

std::string MCPClient::refresh() {
    list_changed_ = false;
    std::string err;
    err = discover_tools();
    if (err.empty())
        err = discover_resources();
    if (err.empty())
        err = discover_prompts();
    if (!err.empty())
        error_ = err;
    return err;
}

void MCPClient::disconnect() {
    if (transport_)
        transport_->shutdown();
    connected_ = false;
    tools_.clear();
    resources_.clear();
    prompts_.clear();
}

McpTransportResult MCPClient::request_with_retry(int id, const std::string& method,
                                                 const json& params) {
    auto r = transport_->request(id, method, params);
    if (r.status != McpTransportStatus::SessionExpired)
        return r;
    // Session lost: re-initialize + re-discover, then retry the request once.
    if (!do_connect().empty())
        return r;
    return transport_->request(id, method, params);
}

namespace {

McpResult failure(const std::string& message) {
    McpResult out;
    out.ok = false;
    out.error = message;
    return out;
}

bool cancel_requested(const CancellationToken* token) {
    return token && token->is_requested();
}

} // namespace

McpResult MCPClient::call_tool(const std::string& name, const json& arguments) {
    if (!connected_ || !transport_)
        return failure("mcp server '" + name_ + "' not connected");
    if (cancel_requested(cancel_token_))
        return failure("cancelled by user");
    if (list_changed_)
        refresh();

    const int req_id = next_id();
    const auto r =
        request_with_retry(req_id, "tools/call", {{"name", name}, {"arguments", arguments}});
    if (cancel_requested(cancel_token_) || r.status == McpTransportStatus::Cancelled) {
        notify_cancelled(req_id);
        return failure("cancelled by user");
    }
    if (r.status == McpTransportStatus::Timeout)
        return failure("mcp call timed out");
    if (r.status == McpTransportStatus::TransportError || !r.message)
        return failure(transport_->failure_reason().empty() ? "mcp call failed"
                                                            : transport_->failure_reason());
    if (r.message->error)
        return failure(r.message->error->to_text());

    McpResult out;
    const json result = r.message->result.value_or(json::object());
    out.ok = !result.value("isError", false);
    out.text = mcp_flatten_content(result.value("content", json::array()), kToolCap);
    if (!out.ok)
        out.error = out.text.empty() ? "tool returned isError" : out.text;
    return out;
}

McpResult MCPClient::read_resource(const std::string& uri) {
    McpResult out;
    if (!connected_ || !transport_) {
        out.ok = false;
        out.error = "mcp server '" + name_ + "' not connected";
        return out;
    }
    if (list_changed_)
        refresh();
    auto r = request_with_retry(next_id(), "resources/read", {{"uri", uri}});
    if (r.status == McpTransportStatus::Timeout) {
        out.ok = false;
        out.error = "mcp resource read timed out";
        return out;
    }
    if (r.status == McpTransportStatus::TransportError || !r.message) {
        out.ok = false;
        out.error = transport_->failure_reason().empty() ? "mcp resource read failed"
                                                         : transport_->failure_reason();
        return out;
    }
    if (r.message->error) {
        out.ok = false;
        out.error = r.message->error->to_text();
        return out;
    }
    json result = r.message->result.value_or(json::object());
    out.text = mcp_flatten_content(result.value("contents", json::array()), kResourceCap);
    return out;
}

namespace {

// Flatten a prompts/get result into "role: text" lines, capped.
std::string flatten_prompt_messages(const json& result) {
    std::string flat;
    for (const auto& m : result.value("messages", json::array())) {
        const std::string role = m.value("role", "user");
        const std::string text = m.value("content", json::object()).value("text", "");
        std::string line = role;
        line += ": ";
        line += text;
        append_capped(flat, line, kPromptCap);
        if (flat.empty() || flat.back() != '\n')
            flat += "\n";
    }
    return flat;
}

} // namespace

McpResult MCPClient::get_prompt(const std::string& name, const json& arguments) {
    if (!connected_ || !transport_)
        return failure("mcp server '" + name_ + "' not connected");
    if (list_changed_)
        refresh();
    const auto r =
        request_with_retry(next_id(), "prompts/get", {{"name", name}, {"arguments", arguments}});
    if (r.status == McpTransportStatus::Timeout)
        return failure("mcp prompt get timed out");
    if (r.status == McpTransportStatus::TransportError || !r.message)
        return failure(transport_->failure_reason().empty() ? "mcp prompt get failed"
                                                            : transport_->failure_reason());
    if (r.message->error)
        return failure(r.message->error->to_text());

    McpResult out;
    out.text = flatten_prompt_messages(r.message->result.value_or(json::object()));
    return out;
}

std::string MCPClient::discover_tools() {
    tools_.clear();
    if (!caps_.has_tools)
        return "";
    json cursor;
    for (int page = 0; page < kMaxPages; ++page) {
        json params = json::object();
        if (!cursor.is_null())
            params["cursor"] = cursor;
        auto r = request_with_retry(next_id(), "tools/list", params);
        if (r.status != McpTransportStatus::Ok || !r.message) {
            return transport_->failure_reason().empty() ? "tools/list failed"
                                                        : transport_->failure_reason();
        }
        if (r.message->error)
            return r.message->error->to_text();
        json result = r.message->result.value_or(json::object());
        for (const auto& t : result.value("tools", json::array())) {
            McpToolDef def;
            def.name = t.value("name", "");
            def.title = t.value("title", "");
            def.description = t.value("description", "");
            def.input_schema = t.value("inputSchema", json::object());
            if (!def.name.empty())
                tools_.push_back(std::move(def));
        }
        if (!result.contains("nextCursor"))
            break;
        cursor = result["nextCursor"];
    }
    return "";
}

std::string MCPClient::discover_resources() {
    resources_.clear();
    if (!caps_.has_resources)
        return "";
    json cursor;
    for (int page = 0; page < kMaxPages; ++page) {
        json params = json::object();
        if (!cursor.is_null())
            params["cursor"] = cursor;
        auto r = request_with_retry(next_id(), "resources/list", params);
        if (r.status != McpTransportStatus::Ok || !r.message) {
            return transport_->failure_reason().empty() ? "resources/list failed"
                                                        : transport_->failure_reason();
        }
        if (r.message->error)
            return r.message->error->to_text();
        json result = r.message->result.value_or(json::object());
        for (const auto& res : result.value("resources", json::array())) {
            McpResourceDef def;
            def.uri = res.value("uri", "");
            def.name = res.value("name", "");
            def.description = res.value("description", "");
            def.mime_type = res.value("mimeType", "");
            if (!def.uri.empty())
                resources_.push_back(std::move(def));
        }
        if (!result.contains("nextCursor"))
            break;
        cursor = result["nextCursor"];
    }
    return "";
}

std::string MCPClient::discover_prompts() {
    prompts_.clear();
    if (!caps_.has_prompts)
        return "";
    json cursor;
    for (int page = 0; page < kMaxPages; ++page) {
        json params = json::object();
        if (!cursor.is_null())
            params["cursor"] = cursor;
        auto r = request_with_retry(next_id(), "prompts/list", params);
        if (r.status != McpTransportStatus::Ok || !r.message) {
            return transport_->failure_reason().empty() ? "prompts/list failed"
                                                        : transport_->failure_reason();
        }
        if (r.message->error)
            return r.message->error->to_text();
        json result = r.message->result.value_or(json::object());
        for (const auto& p : result.value("prompts", json::array())) {
            McpPromptDef def;
            def.name = p.value("name", "");
            def.title = p.value("title", "");
            def.description = p.value("description", "");
            for (const auto& a : p.value("arguments", json::array())) {
                if (a.value("required", false))
                    def.required_args.push_back(a.value("name", ""));
            }
            if (!def.name.empty())
                prompts_.push_back(std::move(def));
        }
        if (!result.contains("nextCursor"))
            break;
        cursor = result["nextCursor"];
    }
    return "";
}

void MCPClient::handle_server_message(const McpMessage& msg) {
    if (msg.is_response())
        return;
    if (msg.method == "notifications/tools/list_changed" ||
        msg.method == "notifications/resources/list_changed" ||
        msg.method == "notifications/prompts/list_changed") {
        list_changed_ = true;
        return;
    }
    if (msg.method == "notifications/logging/message") {
        return; // v1: ignored (debug log wiring is the manager's concern)
    }
    if (!msg.is_notification() && msg.id.has_value() && transport_) {
        McpError err;
        err.code = kMcpErrMethodNotFound;
        err.message = "method not supported by amber client: " + msg.method;
        transport_->respond_error(msg.id->is_number_integer() ? msg.id->get<int>() : 0, err);
    }
}

std::string mcp_flatten_content(const json& content, size_t cap_bytes) {
    std::string out;
    for (const auto& block : content) {
        if (!block.is_object())
            continue;
        std::string type = block.value("type", "");
        if (type == "text" || (type.empty() && block.contains("text"))) {
            append_capped(out, block.value("text", ""), cap_bytes);
        } else if (type == "resource") {
            json res = block.value("resource", json::object());
            std::string text = res.value("text", "");
            std::string uri = res.value("uri", "");
            std::string prefix =
                uri.empty() ? "[embedded resource]\n" : "[resource: " + uri + "]\n";
            append_capped(out, prefix, cap_bytes);
            if (!text.empty())
                append_capped(out, text, cap_bytes);
        } else if (type == "image" || type == "audio") {
            std::string mime = block.value("mimeType", type);
            std::string data = block.value("data", "");
            std::string note = "[";
            note += type;
            note += " ";
            note += mime;
            note += ", ";
            note += std::to_string(data.size());
            note += " bytes]\n";
            append_capped(out, note, cap_bytes);
        } else if (type == "resource_link") {
            std::string uri = block.value("uri", "");
            append_capped(out, "[resource link: " + uri + "]\n", cap_bytes);
        } else if (type.empty() && block.contains("blob")) {
            std::string data = block.value("blob", "");
            append_capped(out, "[binary resource, " + std::to_string(data.size()) + " bytes]\n",
                          cap_bytes);
        }
    }
    return out;
}

void MCPClient::notify_cancelled(int request_id) {
    if (!transport_)
        return;
    transport_->notify("notifications/cancelled",
                       {{"requestId", request_id}, {"reason", "user cancelled"}});
}

} // namespace agent

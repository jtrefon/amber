
#ifndef AGENT_MCP_TRANSPORT_H
#define AGENT_MCP_TRANSPORT_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace agent {

// JSON-RPC 2.0 error codes used by MCP.
constexpr int kMcpErrParse = -32700;
constexpr int kMcpErrInvalidRequest = -32600;
constexpr int kMcpErrMethodNotFound = -32601;
constexpr int kMcpErrInvalidParams = -32602;
constexpr int kMcpErrInternal = -32603;

// A JSON-RPC error object (response errors, and -32000..-32099 server errors).
struct McpError {
    int code = 0;
    std::string message;
    json data = json::object();

    // Human-readable form for ToolResult error strings.
    std::string to_text() const;
};

// An outgoing request (id + method + params).
struct McpRequest {
    int id = 0;
    std::string method;
    json params = json::object();
};

// One decoded JSON-RPC 2.0 message. Requests and notifications carry
// `method`; responses carry `result` or `error`. Ids may be integers or
// strings on the wire; they are decoded into `json` for tolerance.
struct McpMessage {
    std::optional<json> id;
    std::string method;
    json params = json::object();
    std::optional<json> result;
    std::optional<McpError> error;

    bool is_notification() const { return !method.empty() && !id.has_value(); }
    bool is_response() const { return id.has_value() && method.empty(); }
};

// Encode helpers: compact JSON, single line, no raw newlines (stdio framing
// invariant — payload newlines are JSON-escaped). Never throw.
std::string mcp_encode_request(const McpRequest& req);
std::string mcp_encode_notification(const std::string& method,
                                    const json& params);
std::string mcp_encode_response(int id, const json& result);
std::string mcp_encode_error_response(int id, const McpError& error);

// Parse one JSON-RPC line. Tolerant: unknown fields are ignored, string ids
// are accepted. Returns nullopt for invalid JSON, non-objects, or messages
// with neither a method nor an id (transport failure at the caller).
std::optional<McpMessage> mcp_decode_line(const std::string& line);

// Outcome of a request: Ok with a message, or a typed failure the client can
// act on. SessionExpired means the remote session is gone and the client
// should re-initialize and retry once.
enum class McpTransportStatus : std::uint8_t {
    Ok,
    Timeout,
    SessionExpired,
    TransportError,
};

struct McpTransportResult {
    std::optional<McpMessage> message;
    McpTransportStatus status = McpTransportStatus::Ok;
};

// The transport port: one live connection to one MCP server. Implementations:
// StdioTransport (spawned subprocess) and HttpTransport (remote endpoint).
// Server-initiated requests and notifications are delivered through the
// callback registered at construction.
class McpTransport {
public:
    explicit McpTransport(
        std::function<void(const McpMessage&)> on_server_message = {})
        : on_server_message_(std::move(on_server_message)) {}
    virtual ~McpTransport() = default;
    McpTransport(const McpTransport&) = delete;
    McpTransport& operator=(const McpTransport&) = delete;

    // Send a request and wait for the matching response (or error response).
    virtual McpTransportResult request(int id, const std::string& method,
                                       const json& params) = 0;

    // Send a notification; returns false on transport failure.
    virtual bool notify(const std::string& method, const json& params) = 0;

    // Respond to a server-initiated request.
    virtual bool respond(int id, const json& result) = 0;
    virtual bool respond_error(int id, const McpError& error) = 0;

    // End the server session gracefully where the transport supports it
    // (HTTP DELETE). No-op for stdio.
    virtual void close_session() {}

    // Blocking teardown; idempotent. Called by the owning client.
    virtual void shutdown() = 0;

    // Empty while healthy; otherwise a human-readable reason the transport
    // cannot be used (spawn failure, disconnect, protocol violation). May
    // include a capped tail of the server's stderr.
    virtual std::string failure_reason() const = 0;

    // (Re)bind the server-message callback (used when the transport is
    // constructed before its owner exists).
    void set_on_server_message(
        std::function<void(const McpMessage&)> cb) {
        on_server_message_ = std::move(cb);
    }

protected:
    std::function<void(const McpMessage&)> on_server_message_;
};

} // namespace agent

#endif // AGENT_MCP_TRANSPORT_H

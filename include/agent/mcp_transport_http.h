
#ifndef AGENT_MCP_TRANSPORT_HTTP_H
#define AGENT_MCP_TRANSPORT_HTTP_H

#include <functional>
#include <string>

#include "agent/mcp_transport.h"

namespace agent {

// MCP Streamable HTTP transport (protocol 2025-06-18): every JSON-RPC message
// is an HTTP POST to a single endpoint; replies are either application/json
// or text/event-stream (SSE). The transport captures the Mcp-Session-Id from
// the initialize response and echoes it (plus MCP-Protocol-Version and an
// optional Authorization: Bearer token) on every request. A 404 with a
// session id surfaces as McpTransportStatus::SessionExpired so the client can
// re-initialize and retry once.
class HttpTransport : public McpTransport {
public:
    HttpTransport(std::string url, std::string auth_token = "",
                  int request_timeout_ms = 60000,
                  std::function<void(const McpMessage&)> on_server_message =
                      {});

    McpTransportResult request(int id, const std::string& method,
                               const json& params) override;
    bool notify(const std::string& method, const json& params) override;
    bool respond(int id, const json& result) override;
    bool respond_error(int id, const McpError& error) override;
    void close_session() override;
    void shutdown() override;
    std::string failure_reason() const override;

    // The session id captured from the initialize response ("" if the server
    // assigns none).
    const std::string& session_id() const { return session_id_; }

    // Raw HTTP reply captured during a transfer (used by the curl callbacks).
    struct HttpReply {
        long status = 0;
        std::string content_type;
        std::string body;
        std::string session_header;
        bool got_session_header = false;
    };

private:
    // POST one JSON-RPC message and return the HTTP reply. Returns false on
    // transport-level failure (curl error, timeout).
    bool post(const std::string& payload, HttpReply& reply);

    std::string url_;
    std::string auth_token_;
    int request_timeout_ms_;
    std::string session_id_;
    std::string failure_;
    bool closed_ = false;
};

} // namespace agent

#endif // AGENT_MCP_TRANSPORT_HTTP_H

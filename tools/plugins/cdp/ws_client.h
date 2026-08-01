// Minimal RFC 6455 WebSocket client over POSIX sockets. Dependency-free on
// purpose: the CDP plugin uses it to talk to Chrome, and the core app never
// links it. Text messages only (CDP is JSON-RPC over text frames); ping is
// answered with pong, close is handled, fragmentation is handled by
// reassembly across reads.
#ifndef CDP_WS_CLIENT_H
#define CDP_WS_CLIENT_H

#include <string>

namespace cdp {

class WsClient {
public:
    WsClient() = default;
    ~WsClient() { close(); }

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // Connect to ws://host:port/path and complete the handshake. Returns
    // false and fills `err` on failure.
    bool connect(const std::string& url, std::string& err);

    // Send one text frame.
    bool send_text(const std::string& payload);

    // Receive one text frame within timeout_ms. Frames of other types are
    // handled internally (ping → pong, close → fail). Returns false on
    // timeout, closed connection, or protocol error.
    bool recv_text(std::string& out, int timeout_ms);

    void close();

    bool connected() const { return fd_ >= 0; }

    // HTTP GET for http(s):// or ws:// URLs; returns the response body.
    bool http_get_url(const std::string& url, std::string& body,
                      std::string& err);

private:
    bool parse_url(const std::string& url, std::string& host, int& port,
                   std::string& path);
    bool http_get(const std::string& host, int port, const std::string& req_path,
                  std::string& body, std::string& err);
    bool read_some(int timeout_ms);
    bool pump_frame(std::string& out);
    void send_frame(int opcode, const std::string& payload) const;

    int fd_ = -1;
    std::string buf_;  // raw bytes read but not yet framed
};

} // namespace cdp

#endif // CDP_WS_CLIENT_H

// Minimal MCP Streamable HTTP test server (C++ replacement for
// mcp_http_server.py), so the test suite has no Python dependency.
//
// Usage: mcp_http <statefile> [mode]
// Writes "PORT:<port>\nPID:<pid>\n" to <statefile> once listening.
//
// Modes:
//   echo     — answer every request with application/json {echo: {...}}
//   sse      — answer requests with text/event-stream: a progress notification,
//              then the response, then close
//   session  — assign Mcp-Session-Id "sess-1" on initialize; require it on
//              subsequent requests (400 otherwise); expire after 3 requests (404)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr const char* kSessionId = "sess-1";
std::atomic<int> g_counter{0};

std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

std::string header_value(const std::map<std::string, std::string>& headers,
                         const std::string& name) {
    auto it = headers.find(to_lower(name));
    return it == headers.end() ? std::string() : it->second;
}

// Reads a full HTTP request (headers + Content-Length body) off one socket.
bool read_request(int fd, std::string& method, std::string& path,
                  std::string& body,
                  std::map<std::string, std::string>& headers) {
    std::string buf;
    char tmp[4096];
    const std::string term = "\r\n\r\n";
    while (buf.find(term) == std::string::npos) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
    const size_t body_off = buf.find(term) + term.size();
    const std::string head = buf.substr(0, body_off - term.size());

    const size_t sp1 = head.find(' ');
    const size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                                  : head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    method = head.substr(0, sp1);
    path = head.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
        const size_t line_start = pos + 2;
        const size_t nl = head.find("\r\n", line_start);
        const size_t line_end =
            (nl == std::string::npos) ? head.size() : nl;
        const std::string line = head.substr(line_start, line_end - line_start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            headers[to_lower(line.substr(0, colon))] = value;
        }
        if (nl == std::string::npos) break;
        pos = nl;
    }

    long clen = 0;
    const std::string len = header_value(headers, "Content-Length");
    if (!len.empty()) clen = std::atol(len.c_str());
    while (buf.size() < body_off + static_cast<size_t>(clen)) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
    }
    body = buf.substr(body_off, static_cast<size_t>(clen));
    return true;
}

void send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(fd, s.data() + off, s.size() - off);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

void respond(int fd, int status, const char* reason,
             const std::string& content_type, const std::string& body,
             const std::string& extra_header = "") {
    std::string h = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                    "\r\n";
    if (!content_type.empty())
        h += "Content-Type: " + content_type + "\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!extra_header.empty()) h += extra_header + "\r\n";
    h += "\r\n";
    send_all(fd, h + body);
}

void respond_json(int fd, int status, const char* reason, const std::string& body,
                  const std::string& extra_header = "") {
    respond(fd, status, reason, "application/json", body, extra_header);
}

void respond_accepted(int fd) {
    respond(fd, 202, "Accepted", "", "");
}

json init_result() {
    return {{"protocolVersion", "2025-06-18"},
            {"capabilities",
             {{"tools", {{"listChanged", true}}},
              {"resources", json::object()},
              {"prompts", json::object()}}},
            {"serverInfo", {{"name", "fixture"}, {"version", "1.0"}}}};
}

void handle_post(int fd, const std::string& mode, const std::string& body,
                 const std::map<std::string, std::string>& headers) {
    g_counter++;
    json obj = json::parse(body, nullptr, false);
    if (obj.is_discarded()) {
        respond_json(fd, 400, "Bad Request",
                     "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,"
                     "\"message\":\"parse error\"}}");
        return;
    }
    const std::string method = obj.value("method", std::string());
    json reqid = obj.contains("id") ? obj["id"] : json();
    const std::string session = header_value(headers, "Mcp-Session-Id");

    if (mode == "session" && method != "initialize") {
        if (session != kSessionId) {
            respond_json(fd, 400, "Bad Request",
                         "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,"
                         "\"message\":\"bad session\"}}");
            return;
        }
        if (g_counter >= 4) {
            respond_json(fd, 404, "Not Found",
                         "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,"
                         "\"message\":\"gone\"}}");
            return;
        }
    }
    if (reqid.is_null()) {
        respond_accepted(fd);
        return;
    }

    if (method == "initialize") {
        json result = init_result();
        std::string extra;
        if (mode == "session") {
            result["sessionSeen"] = session;
            extra = "Mcp-Session-Id: " + std::string(kSessionId);
        }
        respond_json(fd, 200, "OK",
                     json{{"jsonrpc", "2.0"}, {"id", reqid},
                          {"result", result}}.dump(),
                     extra);
        return;
    }

    if (mode == "sse") {
        const std::string payload =
            "event: message\n"
            "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\","
            "\"params\":{\"progressToken\":\"p1\",\"progress\":1}}\n"
            "\n"
            "event: message\n"
            "data: {\"jsonrpc\":\"2.0\",\"id\":" +
            reqid.dump() + ",\"result\":{\"sse\":true,\"session\":" +
            json(session).dump() + "}}\n\n";
        respond(fd, 200, "OK", "text/event-stream", payload);
        return;
    }

    json result = {{"echo", {{"method", method},
                             {"params", obj.value("params", json::object())}}}};
    if (mode == "session") result["session"] = session;
    respond_json(fd, 200, "OK",
                 json{{"jsonrpc", "2.0"}, {"id", reqid},
                      {"result", result}}.dump());
}

void handle_connection(int fd, const std::string& mode) {
    // One request per connection: the MCP HTTP client opens a fresh
    // connection per POST via curl, and the fixture reply closes it.
    std::string method, path, body;
    std::map<std::string, std::string> headers;
    if (read_request(fd, method, path, body, headers)) {
        if (method == "POST") handle_post(fd, mode, body, headers);
        else if (method == "DELETE") respond_accepted(fd);
    }
    close(fd);
}

} // namespace

int main(int argc, char** argv) {
    const std::string statefile = argc > 1 ? argv[1] : "/tmp/mcp_http.txt";
    const std::string mode = argc > 2 ? argv[2] : "echo";

    const int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) return 1;
    int opt = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lsock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
        return 1;
    if (listen(lsock, 16) < 0) return 1;
    socklen_t alen = sizeof addr;
    getsockname(lsock, reinterpret_cast<sockaddr*>(&addr), &alen);

    FILE* f = std::fopen(statefile.c_str(), "w");
    if (f != nullptr) {
        std::fprintf(f, "PORT:%d\nPID:%d\n",
                     static_cast<int>(ntohs(addr.sin_port)),
                     static_cast<int>(getpid()));
        std::fclose(f);
    }

    for (;;) {
        const int c = accept(lsock, nullptr, nullptr);
        if (c < 0) continue;
        std::thread(handle_connection, c, mode).detach();
    }
}
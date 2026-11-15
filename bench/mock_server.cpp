#include "bench/mock_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace bench {

namespace {

std::string completion_json(const MockReply& reply) {
    agent::json message = {{"role", "assistant"}, {"content", reply.content}};
    if (!reply.tool_calls.empty())
        message["tool_calls"] = reply.tool_calls;
    agent::json choice = {{"index", 0},
                          {"message", std::move(message)},
                          {"finish_reason", reply.tool_calls.empty() ? "stop" : "tool_calls"}};
    agent::json choices = agent::json::array();
    choices.push_back(std::move(choice));
    agent::json body = {
        {"id", "chatcmpl-mock"},
        {"object", "chat.completion"},
        {"created", 1},
        {"model", "fake"},
        {"choices", std::move(choices)},
        {"usage",
         {{"prompt_tokens", reply.prompt_tokens},
          {"completion_tokens", reply.completion_tokens},
          {"total_tokens", reply.prompt_tokens + reply.completion_tokens}}},
    };
    return body.dump();
}

std::string stream_body(const MockReply& reply) {
    agent::json delta = agent::json::object();
    if (!reply.tool_calls.empty())
        delta["tool_calls"] = reply.tool_calls;
    else
        delta["content"] = reply.content;
    agent::json choice = {{"index", 0},
                          {"delta", std::move(delta)},
                          {"finish_reason", reply.tool_calls.empty() ? "stop" : "tool_calls"}};
    agent::json choices = agent::json::array();
    choices.push_back(std::move(choice));
    agent::json chunk = {{"choices", std::move(choices)}};
    return "data: " + chunk.dump() + "\n\ndata: [DONE]\n\n";
}

std::string read_request(int fd) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
            return {};
        request.append(buffer, static_cast<std::size_t>(n));
    }
    const std::size_t header_end = request.find("\r\n\r\n");
    std::size_t content_length = 0;
    const std::size_t cl = request.find("Content-Length:");
    if (cl != std::string::npos && cl < header_end)
        content_length =
            static_cast<std::size_t>(std::strtoul(request.c_str() + cl + 15, nullptr, 10));
    while (request.size() < header_end + 4 + content_length) {
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
            break;
        request.append(buffer, static_cast<std::size_t>(n));
    }
    return request;
}

std::string request_path(const std::string& request) {
    const std::size_t sp1 = request.find(' ');
    const std::size_t sp2 =
        sp1 == std::string::npos ? std::string::npos : request.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos)
        return "/";
    std::string path = request.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = path.find('?');
    if (q != std::string::npos)
        path.resize(q);
    return path;
}

std::string request_body(const std::string& request) {
    const std::size_t header_end = request.find("\r\n\r\n");
    return header_end == std::string::npos ? std::string() : request.substr(header_end + 4);
}

} // namespace

MockServer::MockServer(std::deque<MockReply> script) : script_(std::move(script)) {}

MockServer::~MockServer() {
    stop();
}

bool MockServer::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        return false;
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0 ||
        listen(listen_fd_, 8) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
    return true;
}

void MockServer::stop() {
    stopping_ = true;
    if (thread_.joinable())
        thread_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

std::string MockServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
}

std::vector<std::string> MockServer::paths() const {
    std::scoped_lock lock(paths_mtx_);
    return paths_;
}

bool MockServer::next_reply(MockReply& out) {
    std::scoped_lock lock(script_mtx_);
    if (script_.empty())
        return false;
    out = std::move(script_.front());
    script_.pop_front();
    return true;
}

void MockServer::serve() {
    while (!stopping_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd_, &rfds);
        timeval tv{0, 200000};
        if (select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0)
            continue;
        const int client = accept(listen_fd_, nullptr, nullptr);
        if (client < 0)
            continue;
        handle(client);
        close(client);
    }
}

void MockServer::handle(int fd) {
    const std::string request = read_request(fd);
    if (request.empty())
        return;
    const std::string path = request_path(request);
    {
        std::scoped_lock lock(paths_mtx_);
        paths_.emplace_back(path);
    }
    if (path == "/v1/models") {
        const std::string models =
            R"({"data":[{"id":"fake","context_length":8192,"top_provider":{"max_completion_tokens":4096},"supported_parameters":["tools"]}]})";
        send_all(fd, models, "application/json");
        return;
    }
    if (path == "/v1/chat/completions") {
        MockReply reply;
        if (!next_reply(reply))
            reply.content = "done";
        const bool streaming = request_body(request).find("\"stream\":true") != std::string::npos;
        send_all(fd, streaming ? stream_body(reply) : completion_json(reply),
                 streaming ? "text/event-stream" : "application/json");
        return;
    }
    send_all(fd, R"({"error":{"message":"not found"}})", "application/json");
}

void MockServer::send_all(int fd, const std::string& body, const char* content_type) {
    const std::string response = std::string("HTTP/1.1 200 OK\r\nContent-Type: ") + content_type +
                                 "\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t n = send(fd, response.c_str() + sent, response.size() - sent, 0);
        if (n <= 0)
            return;
        sent += static_cast<std::size_t>(n);
    }
}

} // namespace bench

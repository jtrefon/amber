#include "bench/scripted_endpoint.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace bench {

namespace {

std::string lower(std::string value) {
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
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
    const std::string headers = lower(request.substr(0, header_end));
    const std::size_t cl = headers.find("content-length:");
    if (cl != std::string::npos) {
        const std::size_t start = cl + std::strlen("content-length:");
        content_length =
            static_cast<std::size_t>(std::strtoul(headers.c_str() + start, nullptr, 10));
    }
    while (request.size() < header_end + 4 + content_length) {
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
            break;
        request.append(buffer, static_cast<std::size_t>(n));
    }
    return request;
}

std::string request_method(const std::string& request) {
    const std::size_t end = request.find(' ');
    return end == std::string::npos ? std::string() : request.substr(0, end);
}

std::string request_target(const std::string& request) {
    const std::size_t first = request.find(' ');
    if (first == std::string::npos)
        return "/";
    const std::size_t second = request.find(' ', first + 1);
    if (second == std::string::npos)
        return "/";
    return request.substr(first + 1, second - first - 1);
}

std::string request_path(const std::string& target) {
    const std::size_t query = target.find('?');
    return query == std::string::npos ? target : target.substr(0, query);
}

std::string request_body(const std::string& request) {
    const std::size_t header_end = request.find("\r\n\r\n");
    return header_end == std::string::npos ? std::string() : request.substr(header_end + 4);
}

std::map<std::string, std::string> request_headers(const std::string& request) {
    std::map<std::string, std::string> out;
    const std::size_t header_end = request.find("\r\n\r\n");
    const std::string block = request.substr(0, header_end);
    std::size_t start = block.find("\r\n");
    if (start == std::string::npos)
        return out;
    start += 2;
    while (start < block.size()) {
        const std::size_t end = block.find("\r\n", start);
        const std::string line =
            block.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos)
            out[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        if (end == std::string::npos)
            break;
        start = end + 2;
    }
    return out;
}

std::string status_text(int status) {
    switch (status) {
    case 200:
        return "200 OK";
    case 400:
        return "400 Bad Request";
    case 404:
        return "404 Not Found";
    case 429:
        return "429 Too Many Requests";
    case 500:
        return "500 Internal Server Error";
    default:
        return std::to_string(status) + " Error";
    }
}

std::string error_body(const std::string& message) {
    return agent::json{{"error", {{"message", message}}}}.dump();
}

std::string completion_json(const EndpointReply& reply) {
    agent::json message = {{"role", "assistant"}, {"content", reply.content}};
    if (!reply.tool_calls.empty())
        message["tool_calls"] = reply.tool_calls;
    if (!reply.reasoning.empty())
        message["reasoning_content"] = reply.reasoning;
    const std::string finish = reply.finish_reason.empty()
                                   ? (reply.tool_calls.empty() ? "stop" : "tool_calls")
                                   : reply.finish_reason;
    agent::json choice = {{"index", 0}, {"message", std::move(message)}, {"finish_reason", finish}};
    agent::json choices = agent::json::array();
    choices.push_back(std::move(choice));
    const agent::json body = {
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

std::string openai_sse_chunk(const EndpointReply& reply, bool content) {
    agent::json delta = agent::json::object();
    if (content && !reply.tool_calls.empty())
        delta["tool_calls"] = reply.tool_calls;
    else if (content)
        delta["content"] = reply.content;
    if (!reply.reasoning.empty())
        delta["reasoning_content"] = reply.reasoning;
    const agent::json option = {
        {"index", 0},
        {"delta", std::move(delta)},
        {"finish_reason", content ? (reply.tool_calls.empty() ? "stop" : "tool_calls") : nullptr}};
    agent::json choices = agent::json::array();
    choices.push_back(option);
    return "data: " + agent::json{{"choices", std::move(choices)}}.dump() + "\n\n";
}

std::string stream_body(const EndpointReply& reply) {
    std::vector<std::string> chunks = reply.sse_chunks;
    if (chunks.empty()) {
        chunks.push_back(openai_sse_chunk(reply, true));
        chunks.emplace_back("data: [DONE]\n\n");
    }
    const int limit = reply.drop_after_chunks > 0
                          ? std::min<int>(reply.drop_after_chunks, static_cast<int>(chunks.size()))
                          : static_cast<int>(chunks.size());
    std::string body;
    for (int i = 0; i < limit; ++i)
        body += chunks[static_cast<std::size_t>(i)];
    return body;
}

std::string default_models() {
    return R"({"data":[{"id":"fake","context_length":8192,"top_provider":{"max_completion_tokens":4096},"supported_parameters":["tools"]}]})";
}

EndpointRequest parse_endpoint_request(const std::string& raw) {
    const std::string target = request_target(raw);
    EndpointRequest request;
    request.method = request_method(raw);
    request.path = request_path(target);
    request.body = request_body(raw);
    request.headers = request_headers(raw);
    request.json = agent::json::parse(request.body, nullptr, false);
    if (request.json.is_discarded())
        request.json = agent::json::object();
    request.messages = endpoint_messages(request.json);
    if (request.json.is_object() && request.json.contains("tools") &&
        request.json["tools"].is_array())
        request.tools = request.json["tools"];
    const bool body_stream = request.json.is_object() && request.json.value("stream", false);
    request.stream = body_stream || target.find("streamGenerateContent") != std::string::npos ||
                     target.find("alt=sse") != std::string::npos;
    return request;
}

std::string content_from_blocks(const agent::json& content) {
    if (content.is_string())
        return content.get<std::string>();
    std::string out;
    if (!content.is_array())
        return out;
    for (const auto& block : content) {
        if (!block.is_object())
            continue;
        if (block.value("type", std::string()) == "text")
            out += block.value("text", std::string());
        else if (block.contains("text") && block["text"].is_string())
            out += block["text"].get<std::string>();
    }
    return out;
}

} // namespace

bool endpoint_is_chat_path(const std::string& path) {
    return path == "/v1/chat/completions" || path == "/v1/messages" ||
           path.find(":generateContent") != std::string::npos ||
           path.find(":streamGenerateContent") != std::string::npos;
}

namespace {

std::string block_text(const agent::json& block) {
    if (block.is_string())
        return block.get<std::string>();
    if (block.is_object() && block.contains("text") && block["text"].is_string())
        return block["text"].get<std::string>();
    return {};
}

std::string content_text(const agent::json& content) {
    if (content.is_string())
        return content.get<std::string>();
    std::string out;
    if (!content.is_array())
        return out;
    for (const auto& block : content)
        out += block_text(block);
    return out;
}

void append_declared_tools(const agent::json& declarations, std::vector<std::string>& out) {
    if (!declarations.is_array())
        return;
    for (const auto& declaration : declarations)
        if (declaration.is_object() && declaration.contains("name") &&
            declaration["name"].is_string())
            out.push_back(declaration["name"].get<std::string>());
}

} // namespace

std::string endpoint_system_text(const agent::json& body) {
    std::string out;
    if (body.contains("system"))
        out += content_text(body["system"]);
    if (body.contains("systemInstruction") && body["systemInstruction"].is_object())
        out += content_text(body["systemInstruction"].value("parts", agent::json::array()));
    if (!body.contains("messages") || !body["messages"].is_array())
        return out;
    for (const auto& message : body["messages"])
        if (message.is_object() && message.value("role", std::string()) == "system")
            out += content_text(message.value("content", agent::json::object()));
    return out;
}

std::vector<std::string> endpoint_roles(const agent::json& body) {
    std::vector<std::string> roles;
    if (body.contains("system") || body.contains("systemInstruction"))
        roles.emplace_back("system");
    const char* key = body.contains("contents") ? "contents" : "messages";
    if (!body.contains(key) || !body[key].is_array())
        return roles;
    for (const auto& message : body[key])
        if (message.is_object())
            roles.push_back(message.value("role", std::string("user")));
    return roles;
}

std::vector<std::string> endpoint_tool_names(const agent::json& body) {
    std::vector<std::string> out;
    if (!body.contains("tools") || !body["tools"].is_array())
        return out;
    for (const auto& tool : body["tools"]) {
        if (!tool.is_object())
            continue;
        if (tool.contains("function") && tool["function"].is_object()) {
            const agent::json& fn = tool["function"];
            if (fn.contains("name") && fn["name"].is_string())
                out.push_back(fn["name"].get<std::string>());
        } else if (tool.contains("name") && tool["name"].is_string()) {
            out.push_back(tool["name"].get<std::string>());
        }
        append_declared_tools(tool.value("functionDeclarations", agent::json::array()), out);
        append_declared_tools(tool.value("function_declarations", agent::json::array()), out);
    }
    return out;
}

std::vector<agent::Message> endpoint_messages(const agent::json& body) {
    std::vector<agent::Message> out;
    if (!body.is_object())
        return out;
    if (body.contains("messages") && body["messages"].is_array()) {
        for (const auto& item : body["messages"]) {
            if (!item.is_object())
                continue;
            agent::Message message;
            message.role = item.value("role", std::string());
            if (item.contains("content"))
                message.content = content_from_blocks(item["content"]);
            if (item.contains("tool_calls") && item["tool_calls"].is_array())
                message.tool_calls = item["tool_calls"];
            message.tool_call_id = item.value("tool_call_id", std::string());
            message.name = item.value("name", std::string());
            message.reasoning =
                item.value("reasoning_content", item.value("reasoning", std::string()));
            out.push_back(std::move(message));
        }
        return out;
    }
    if (body.contains("contents") && body["contents"].is_array()) {
        for (const auto& item : body["contents"]) {
            if (!item.is_object() || !item.contains("parts"))
                continue;
            agent::Message message;
            message.role = item.value("role", std::string("user"));
            message.content = content_from_blocks(item["parts"]);
            out.push_back(std::move(message));
        }
    }
    return out;
}

ScriptedEndpoint::ScriptedEndpoint(std::deque<EndpointReply> script) : script_(std::move(script)) {}

ScriptedEndpoint::ScriptedEndpoint(EndpointResponder responder)
    : responder_(std::move(responder)) {}

ScriptedEndpoint::~ScriptedEndpoint() {
    stop();
}

bool ScriptedEndpoint::start() {
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

void ScriptedEndpoint::stop() {
    stopping_ = true;
    if (thread_.joinable())
        thread_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

std::string ScriptedEndpoint::root_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

std::string ScriptedEndpoint::base_url() const {
    return root_url() + "/v1";
}

std::vector<std::string> ScriptedEndpoint::paths() const {
    std::scoped_lock lock(log_mtx_);
    return paths_;
}

std::vector<EndpointRequest> ScriptedEndpoint::requests() const {
    std::scoped_lock lock(log_mtx_);
    return requests_;
}

int ScriptedEndpoint::chat_calls() const {
    std::scoped_lock lock(log_mtx_);
    return chat_calls_;
}

void ScriptedEndpoint::reset_chat() {
    std::scoped_lock lock(log_mtx_);
    chat_calls_ = 0;
}

EndpointReply ScriptedEndpoint::next_reply() {
    std::scoped_lock lock(script_mtx_);
    if (script_.empty()) {
        EndpointReply reply;
        reply.content = "done";
        return reply;
    }
    EndpointReply reply = std::move(script_.front());
    script_.pop_front();
    return reply;
}

EndpointReply ScriptedEndpoint::respond(const EndpointRequest& request) {
    if (responder_) {
        EndpointReply reply = responder_(request);
        if (reply.handled)
            return reply;
    }
    if (request.path == "/v1/models") {
        EndpointReply reply;
        reply.body = default_models();
        return reply;
    }
    if (!endpoint_is_chat_path(request.path)) {
        EndpointReply reply;
        reply.status = 404;
        reply.body = error_body("not found");
        return reply;
    }
    return next_reply();
}

void ScriptedEndpoint::serve() {
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

void ScriptedEndpoint::handle(int fd) {
    const std::string raw = read_request(fd);
    if (raw.empty())
        return;

    EndpointRequest request = parse_endpoint_request(raw);
    {
        std::scoped_lock lock(log_mtx_);
        paths_.push_back(request.path);
        if (endpoint_is_chat_path(request.path))
            request.sequence = chat_calls_++;
    }

    EndpointReply reply;
    try {
        reply = respond(request);
    } catch (const std::exception& error) {
        reply.status = 500;
        reply.body = error_body(error.what());
    }

    {
        std::scoped_lock lock(log_mtx_);
        requests_.push_back(request);
    }
    send_reply(fd, request, reply);
}

void ScriptedEndpoint::send_reply(int fd, const EndpointRequest& request,
                                  const EndpointReply& reply) {
    if (reply.disconnect)
        return;
    if (reply.delay_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(reply.delay_ms));
    if (reply.malformed_json) {
        send_all(fd, "{bad", "application/json", 200, 0);
        return;
    }
    if (reply.status != 200) {
        send_all(fd, reply.body.empty() ? error_body("scripted failure") : reply.body,
                 reply.content_type, reply.status, reply.retry_after_s);
        return;
    }
    if (!reply.body.empty()) {
        send_all(fd, reply.body, reply.content_type, 200, 0);
        return;
    }
    if (request.stream) {
        send_all(fd, stream_body(reply), "text/event-stream", 200, 0);
        return;
    }
    send_all(fd, completion_json(reply), "application/json", 200, 0);
}

void ScriptedEndpoint::send_all(int fd, const std::string& body, const std::string& content_type,
                                int status, int retry_after_s) {
    std::string response = "HTTP/1.1 " + status_text(status) + "\r\nContent-Type: " + content_type +
                           "\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n";
    if (retry_after_s > 0)
        response += "Retry-After: " + std::to_string(retry_after_s) + "\r\n";
    response += "\r\n" + body;
    std::size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t n = send(fd, response.c_str() + sent, response.size() - sent, 0);
        if (n <= 0)
            return;
        sent += static_cast<std::size_t>(n);
    }
}

} // namespace bench
